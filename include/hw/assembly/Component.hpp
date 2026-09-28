#pragma once

#include <string>

#include <hw/type/TypeList.hpp>
#include <hw/type/NamedType.hpp>
#include <hw/utility/Clock.hpp>
#include <hw/assembly/Timer.hpp>

namespace hw::assembly {

template<typename Component, type::NameTag Name, typename InputMessageList, typename Traits>
struct ComponentBase : public type::NamedType<Name, ComponentBase<Component, Name, InputMessageList, Traits>> {
	using Dispatcher		= typename Traits::Dispatcher;
	using AppContext		= typename Dispatcher::AppContextType;
	using EtherMsgList	= typename Dispatcher::EtherMsgList;
	using InputMsgList  = InputMessageList;
  using LocalClock    = utility::SystemClockTSC;

  static_assert(mp_is_set<InputMsgList>::value, "Input list cannot have duplicates");
  static_assert(mp_size<mp_set_difference<InputMsgList, EtherMsgList>>::value == 0,
                "Input list must be subset of Ether message list");

ComponentBase(Dispatcher &dispatcher, AppContext & context)
  : _dispatcher (dispatcher), _context(context), _clock(_dispatcher.clock()), _name (Name) {
  }

  ~ComponentBase() {
  }

  ComponentBase (const ComponentBase &) = delete;
  ComponentBase & operator = (const ComponentBase &) = delete;

  template <typename Type>
  Type getAttribute(const std::string & object, const std::string & attribute, const std:: string & defval) const {
    return _context.template getAttribute<Type>(object, attribute, defval);
  }

  const std:: string & name() const {
    return _name;
  }

  template <typename MsgType>
  struct ToCall {
    static constexpr bool value = (mp_find<InputMsgList, MsgType>::value < mp_size<InputMsgList>::value);
  };

  template <typename MsgType>
  void forwardMsg(const MsgType & msg) {
    static_cast<Component *>(this)-> processMsg(msg);
  }

  template <typename MsgType, typename ... Args>
  MsgType & allocMsg(Args &&... args) noexcept {
    return _dispatcher.template allocMsg<MsgType>(std::forward<Args>(args) ...);
  }

  template <typename MsgType>
  bool commitMsg(MsgType & msg) noexcept {
      return _dispatcher.template commitMsg (msg);
  }

  template <typename EtherType>
  std::shared_ptr<EtherType> getEther() {
    return _dispatcher.template getEther<EtherType>();
  }

  void setTimer(std::chrono::system_clock::time_point when, std::function<void()> callback) {
    _dispatcher.setTimer(when, std::move(callback));
  }

  template <typename Rep, typename Period>
  void setTimer(TimerType type, std::chrono::duration<Rep, Period> wait, std::function<void()> callback) {
    _dispatcher.setTimer(type, wait, std::move(callback));
  }

  LocalClock & clock () const { return _clock; }

  void processBegin     () {}
  void processEnd       () {}
  void processBatchEnd  () {}

private:
  Dispatcher & _dispatcher;
  AppContext & _context;
  LocalClock & _clock;
  const std::string _name;
};

}
