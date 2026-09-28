#pragma once

#include <concepts>
#include <map>
#include <string>
#include <exception>

#include <hw/utility/MMap.hpp>
#include <hw/utility/Format.hpp>
#include <hw/type/TypeList.hpp>
#include <hw/assembly/Context.hpp>
#include <hw/assembly/Dispatcher.hpp>
#include <hw/assembly/Component.hpp>
#include <hw/utility/Time.hpp>

namespace hw::assembly {

using namespace hw:: utility;


template <typename AppContext, typename Ether, typename... Dispatchers>
class Compartment {
public:
	using AssemblyType = AppContext::Assembly;
	using EtherType	= Ether;
	using DispatcherList = type::type_list<Dispatchers ...>;
  using DispatcherSet = mp_transform<type::make_unique_ptr_t, typename DispatcherList::tuple_type>;
  using LocalClock    = utility::SystemClockTSC;
  static constexpr size_t DISPATCHER_CNT = mp_size<DispatcherList>::value;

  Compartment (AppContext & context, AssemblyType & assembly, Ether & ether)
    : _context (context), _assembly(assembly), _ether(ether), _name(type::TypeName<decltype(*this)>())
  {
  }

  Compartment (const Compartment &) = delete;
  Compartment & operator = (const Compartment &) = delete;

  void initialize() {
    mp_for_each<mp_iota_c<DISPATCHER_CNT>>( [this] (auto idx) {
      using DispatcherType = mp_at_c<DispatcherList, idx>;
      static_assert(std::is_same_v<EtherType, typename DispatcherType::EtherType>, "Ether mismatch");
      std::get<idx>(_dispatchers).reset(new DispatcherType(_assembly, _context, _ether));
    });
  }

  void start() {
    mp_for_each<mp_iota_c<DISPATCHER_CNT>>( [this] (auto idx) {
      std::get<idx>(_dispatchers)->start();
    });
  }

  void stop() {
    mp_for_each<mp_iota_c<DISPATCHER_CNT>>( [this] (auto idx) {
      if (auto dispatcher = std::get<idx>(_dispatchers).get(); dispatcher) {
        dispatcher->stop();
      }
    });
  }

  template <typename EtherType>
  std::shared_ptr<EtherType> getEther() {
    return _assembly.template getEther<EtherType>();
  }

private:
  AppContext &    _context;
  AssemblyType &  _assembly;
  EtherType &     _ether;
  DispatcherSet   _dispatchers;
  std::string     _name;
};


template <typename AppContext, typename... Compartments>
class Assembly {
  static_assert(std::derived_from<AppContext, Context>, "Application context must be subclass of assembly::Context");
  using CompartmentList = type::type_list<Compartments ...>;
  using CompartmentSet = mp_transform<type::make_unique_ptr_t, typename CompartmentList::tuple_type>;

  template <typename CompartmentType>
  using ether_from_compartment_t = typename CompartmentType::EtherType;
  using EtherList   = mp_transform<ether_from_compartment_t, CompartmentList>;
  using EtherSet    = mp_transform<type::make_shared_ptr_t, typename EtherList::tuple_type>;
  using Shmem       = utility::WritableMmap;
  using LocalClock  = utility::SystemClockTSC;

  static constexpr size_t COMPARTMENT_CNT = mp_size<CompartmentList>::value;

public:
  Assembly(AppContext & context) : _context(context)
  {
    mp_for_each<mp_iota_c<COMPARTMENT_CNT>>( [this] (auto idx) {
      // instantiate ether
      using EtherType = mp_at_c<EtherList, idx>;
      std::get<idx>(_ethers).reset(new EtherType());
      EtherType &ether = *std::get<idx>(_ethers);
      uint8_t *buffer = nullptr;
      bool reset = true;
      if constexpr (EtherType:: SHARED_ETHER) {
        // instantiate shared memory
        const std::string etherName(type::TypeName<EtherType>());
        const std::string etherFile = _context.getEther(etherName);
        reset = _context.template getConfig<bool>("ether_init", etherName, "false");
        if (auto it = _shmemfiles.find(etherFile); it != _shmemfiles.end()) {
          throw (std::invalid_argument(
            frmt::format("Invalid shared memory path '{}' for ether '{}';   used by '{}'",
              etherFile, etherName, it->first)));
        }
        _shmemfiles.emplace(etherFile, etherName);
        _shmem[idx].reset(new Shmem(etherFile, EtherType::REQUIRED_MEM_SIZE, reset));
        buffer = _shmem[idx]->data();
      }
      else if (false == std::is_same_v<EtherType, EtherPlaceholder>) {
        buffer = static_cast<uint8_t *>(malloc(EtherType::REQUIRED_MEM_SIZE));
        _buffers.push_back(buffer);
      }
      // initialize ether
      ether.initialize(buffer, EtherType:: REQUIRED_MEM_SIZE, reset);
      // instantiate compartment
      using CompartmentType = mp_at_c<CompartmentList, idx>;
      std::get<idx>(_compartments).reset(new CompartmentType(_context, *this, ether));
    });
  }

  Assembly(const Assembly &) = delete;
  Assembly & operator = (const Assembly &) = delete;

  void initialize() {
    mp_for_each<mp_iota_c<COMPARTMENT_CNT>>( [this] (auto idx) {
      std::get<idx>(_compartments)->initialize();
    });
  }

  void start() {
    mp_for_each<mp_iota_c<COMPARTMENT_CNT>>( [this] (auto idx) {
    std::get<idx>(_compartments)->start();
    });
  }

  void stop() {
    mp_for_each<mp_iota_c<COMPARTMENT_CNT>>( [this] (auto idx) {
      if (auto compartment = std::get<idx>(_compartments).get(); compartment) {
        compartment->stop();
      }
    });
  }

  ~Assembly() {
    stop();
    for (auto ptr : _buffers) {
      ::free(ptr);
    }
  }

  LocalClock & clock() { return _clock; }


  template <typename EtherType>
  std::shared_ptr<EtherType> getEther() {
    constexpr size_t idx = mp_find<EtherList, EtherType>::value;
    static_assert(idx < mp_size<EtherList>::value, "Invalid Ether type");
    return std::get<idx>(_ethers);
  }

private:
  AppContext &                        _context;
  EtherSet                            _ethers;
  CompartmentSet                      _compartments;
  std::unique_ptr<Shmem>              _shmem[COMPARTMENT_CNT];
  std::map<std::string, std::string>  _shmemfiles;
  LocalClock                          _clock;
  std::vector<uint8_t *>              _buffers;
};

}
