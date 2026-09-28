# hw::assembly Developer Guide

## 1. Overview
`hw::assembly` is a high-performance C++ application framework designed for low-latency systems. It provides a structured way to build applications composed of decoupled components that communicate via ring buffers (Ethers) and are executed by pinned threads (Dispatchers).

## 2. Core Concepts

### 2.1 Ether (The Ring Buffer)
The **Ether** is the communication backbone. It is a lock-free, multi-producer, multi-consumer ring buffer.
*   **Role:** Stores messages (POD types) for consumption by components.
*   **Key Feature:** Messages are typed and accessed via a `Cursor`.
*   **Usage:** Messages are allocated directly in the buffer (`allocMsg`) and then committed (`commitMsg`) to become visible to consumers.

### 2.2 Component (The Logic Unit)
A **Component** encapsulates a specific piece of application logic.
*   **Role:** Processes specific message types.
*   **Structure:** Must inherit from `assembly::ComponentBase`.
*   **Message Handling:** Implements `processMsg(const MsgType&)` for every message type it subscribes to.
*   **Lifecycle:** Has hooks for `initialize`, `start`, `stop`, and batch processing events (`processBatchEnd`).

### 2.3 Dispatcher (The Execution Engine)
A **Dispatcher** is a thread that drives a set of Components.
*   **Role:** Reads messages from an Ether, checks timers/IO, and invokes Component handlers.
*   **Traits:** Can be configured with traits (e.g., `DispatcherWithTimer`, `DispatcherWithEpoll`) to enable features like timing or network IO.
*   **Pinning:** Can be pinned to a specific CPU core for consistent latency.

#### 2.3.1 Dispatcher Flavors (Traits)
You can customize the Dispatcher's behavior using **Traits** to match its criticality and deployment model.

*   **Hot Path (Critical):** Runs on an isolated, dedicated CPU core. It never yields the CPU, spinning constantly to process messages with minimal latency.
    ```cpp
    // Default behavior (Critical)
    struct HotPathTraits : assembly::DispatcherWithBatchEnd {};
    ```

*   **Slow Path (Non-Critical):** Runs on a shared CPU core. It yields the CPU when idle to be a "good neighbor" to other processes. Use `DispatcherNonCritical` to enable `std::this_thread::yield()`.
    ```cpp
    // Shared CPU behavior (Non-Critical)
    struct SlowPathTraits : assembly::DispatcherNonCritical, assembly::DispatcherWithTimer {};
    ```

*   **Feature Traits:** Mix and match traits to enable functionality:
    *   `DispatcherWithTimer`: Enables timer support.
    *   `DispatcherWithEpoll`: Enables `EPoller` for network I/O.
    *   `DispatcherWithBatchEnd`: Enables `processBatchEnd` callbacks.

*   **Defining a Custom Dispatcher:**
    ```cpp
    struct MyTraits : assembly::DispatcherWithTimer, assembly::DispatcherNonCritical {};
    using MyDisp = assembly::Dispatcher<"SlowDisp", Context, Ether, Comps, MyTraits>;
    ```

### 2.4 Compartment & Assembly
*   **Compartment:** A grouping of one Ether and one or more Dispatchers that read from it.
*   **Assembly:** The top-level container that manages the lifecycle (init/start/stop) of all Compartments and holds the Application Context.

## 3. Creating an Application

Follow these steps to build a new application:

### Step 3.1: Define Messages
Define the data structures (PODs) that will be passed between components.
```cpp
struct Order { int id; double price; };
struct Trade { int id; double price; };
```

### Step 3.2: Define Components
Create components by inheriting from `ComponentBase` and implementing message handlers.

```cpp
// Define the input message list for this component
using MyInputList = type::type_list<Order>;

struct MyStrategy : public assembly::ComponentBase<MyStrategy, "MyStrategy", MyInputList, MyTraits> {
    using ComponentBase::ComponentBase;

    void processMsg(const Order& msg) {
        // Business logic here
        // Sending a message:
        auto& trade = allocMsg<Trade>();
        trade.id = msg.id;
        commitMsg(trade);
    }
};
```

### Step 3.3: Configure the Assembly
Define the types that make up your application structure using `typedef` or `using`.

1.  **Context:** Define your application context (inheriting from `assembly::Context`).
2.  **Ether:** Define the Ether type with the list of all possible messages.
    ```cpp
    using AllMessages = type::type_list<Order, Trade>;
    using MyEther = assembly::Ether<"MainEther", AllMessages, 1024>;
    ```
3.  **Dispatcher:** Define the Dispatcher type, associating it with the Ether and list of Components.
    ```cpp
    using MyComponents = type::type_list<MyStrategy>;
    using MyDispatcher = assembly::Dispatcher<"MainDisp", MyContext, MyEther, MyComponents>;
    ```
4.  **Assembly:** Tie it all together.
    ```cpp
    using MyCompartment = assembly::Compartment<MyContext, MyEther, MyDispatcher>;
    using MyAssembly = assembly::Assembly<MyContext, MyCompartment>;
    ```

### Step 3.4: Main Execution
Instantiate and start the assembly.

```cpp
int main() {
    MyContext context("MyApp", "config.json");
    MyAssembly app(context);
    
    app.initialize();
    app.start();
    
    // Main thread can wait or perform other tasks
    while(true) sleep(1);
    
    app.stop();
    return 0;
}
```

## 4. Configuration

The framework uses a JSON-based configuration system via `assembly::Config`.
*   **Ethers:** Configure shared memory paths and initialization flags.
*   **Attributes:** Components can read configuration values at runtime using `getAttribute<Type>()`.

### 4.1 Component Configuration via Context
The Application Context serves as a centralized configuration store. You can load configuration from external files (JSON, XML, YAML) and populate the context, which components can then access at runtime using their unique `NameTag`.

**Example Pattern: Configuring multiple `TcpReceiver` instances**

1.  **Define the Component:** Use `getAttribute` with the component's name to fetch specific settings.
    ```cpp
    template <type::NameTag Name>
    struct TcpReceiver : public assembly::ComponentBase<TcpReceiver<Name>, Name, ...> {
        void initialize() {
            // "this->name()" returns the NameTag string (e.g., "FeedA", "FeedB")
            std::string interface = this->template getAttribute<std::string>(this->name(), "interface", "eth0");
            int port = this->template getAttribute<int>(this->name(), "port", 8080);
            connect(interface, port);
        }
    };
    ```

2.  **Instantiate with Unique Names:**
    ```cpp
    using FeedA = TcpReceiver<"FeedA">;
    using FeedB = TcpReceiver<"FeedB">;
    ```

3.  **Populate Context (Main.cpp):** Load your config file and push values to the context.
    ```cpp
    // Assuming config.xml contains:
    // <FeedA interface="10.0.0.1" port="1234"/>
    // <FeedB interface="10.0.0.2" port="5678"/>
    
    MyContext context("MyApp");
    // ... parse XML/YAML ...
    context.setAttribute("FeedA", "interface", "10.0.0.1");
    context.setAttribute("FeedA", "port", "1234");
    context.setAttribute("FeedB", "interface", "10.0.0.2");
    context.setAttribute("FeedB", "port", "5678");
    ```

This pattern allows you to reuse the same component logic while configuring each instance differently based on external deployment settings.

## 5. Performance Best Practices

1.  **Core Pinning:** Dedicate isolated cores to Dispatchers to minimize context switching and cache pollution.
2.  **Batch Processing:** Use `processBatchEnd` for logic that doesn't need to run per-message (e.g., flushing logs or bulk updates).
3.  **Zero-Copy:** The framework uses zero-copy message passing. Always allocate messages via `allocMsg` to construct them directly in the ring buffer.
4.  **POD Messages:** Ensure all messages are Plain Old Data (POD) types to ensure safe storage in shared memory ring buffers.
