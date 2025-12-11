# C++ Learning Guide for Whisper Stream Server

Welcome! This guide will walk you through the C++ code in this project, explaining concepts as we go. By the end, you'll understand what every line does.

## Table of Contents

1. [C++ Basics You'll See](#c-basics-youll-see)
2. [Project Structure](#project-structure)
3. [Understanding the Header File](#understanding-the-header-file)
4. [Understanding the Implementation](#understanding-the-implementation)
5. [The Main Entry Point](#the-main-entry-point)
6. [Memory Management](#memory-management)
7. [Threading and Synchronization](#threading-and-synchronization)
8. [Common Patterns](#common-patterns)

---

## C++ Basics You'll See

### Headers and Includes

```cpp
#include "whisper_server.hpp"  // Our own header (quotes = local file)
#include <iostream>            // Standard library (angle brackets)
#include <vector>              // Standard containers
```

- `#include` copies the contents of a file into your code
- `"quotes"` = look in project directory first
- `<brackets>` = look in system/standard library paths

### Namespaces

```cpp
using json = nlohmann::json;  // Create alias 'json' for the long name
```

Namespaces prevent name collisions. `std::cout` means "cout from the std namespace".

### The `std` Namespace

`std` is the C++ Standard Library namespace. Common things you'll see:

```cpp
std::string      // Text string
std::vector      // Dynamic array (like JavaScript Array)
std::cout        // Print to console
std::cerr        // Print errors
std::thread      // Threading
std::mutex       // Lock for thread safety
std::unique_ptr  // Smart pointer (auto-deletes)
std::shared_ptr  // Smart pointer (reference counted)
```

---

## Project Structure

```
server/src/
├── whisper_server.hpp   // Declarations (what exists)
├── whisper_server.cpp   // Definitions (how it works)
├── audio_buffer.hpp     // Audio buffer declarations
├── audio_buffer.cpp     // Audio buffer implementation
├── main.cpp             // Entry point
└── json.hpp             // Third-party JSON library
```

**Header (.hpp)**: Declares classes, functions, types. Like a "table of contents."
**Implementation (.cpp)**: The actual code that does things.

---

## Understanding the Header File

Let's go through `whisper_server.hpp` piece by piece.

### Include Guards

```cpp
#ifndef WHISPER_SERVER_HPP
#define WHISPER_SERVER_HPP
// ... content ...
#endif
```

This prevents the file from being included twice. Without this, you'd get "redefinition" errors.

### Struct: A Bundle of Data

```cpp
struct ServerConfig {
    std::string model_path = "models/ggml-base.en.bin";
    std::string language = "en";
    int port = 9090;
    int n_contexts = 2;
    bool use_gpu = true;
    // ...
};
```

A `struct` groups related data together. Default values after `=` are used if you don't specify them.

```cpp
// Using defaults
ServerConfig config;  // port = 9090, use_gpu = true, etc.

// Overriding
ServerConfig config;
config.port = 8080;   // Now port = 8080
```

### Forward Declarations

```cpp
struct Session;
class WhisperServer;
```

"Hey compiler, these types exist. I'll define them later." This lets types reference each other.

### Pointers and References

```cpp
whisper_context* ctx = nullptr;  // Pointer: holds memory address, can be null
ContextSlot* context_slot;       // Pointer to a ContextSlot

void foo(const std::string& text);  // Reference: alias to existing object
```

**Pointer (`*`)**:
- Can be `nullptr` (pointing to nothing)
- Must use `->` to access members: `slot->ctx`
- You manage the memory (or use smart pointers)

**Reference (`&`)**:
- Cannot be null
- Use `.` to access members
- Just another name for an existing object

### Smart Pointers

```cpp
std::unique_ptr<AudioBuffer> audio;      // Only ONE owner
std::shared_ptr<Session> session;        // Multiple owners, reference counted
```

Smart pointers automatically delete memory when no longer needed:

```cpp
// unique_ptr: deleted when it goes out of scope
{
    auto ptr = std::make_unique<AudioBuffer>(30.0f, 16000);
    // use ptr...
}  // ptr is automatically deleted here

// shared_ptr: deleted when last reference dies
std::shared_ptr<Session> s1 = std::make_shared<Session>();
std::shared_ptr<Session> s2 = s1;  // Both point to same Session
// Session deleted when both s1 and s2 are gone
```

### Atomic Types

```cpp
std::atomic<bool> active{true};
std::atomic<bool> inference_running{false};
```

`atomic` types are thread-safe for simple operations. Multiple threads can read/write without explicit locking.

```cpp
active = false;           // Thread-safe write
if (active) { ... }       // Thread-safe read
```

### std::function: Storing Callbacks

```cpp
std::function<void(const std::string&)> send_message;
```

This stores a callable thing (function, lambda, etc.) that:
- Returns `void`
- Takes one argument: `const std::string&`

```cpp
// Assign a lambda
session->send_message = [ws](const std::string& msg) {
    ws->send(msg, uWS::OpCode::TEXT);
};

// Call it later
session->send_message("Hello!");  // Sends "Hello!" via WebSocket
```

### Class Definition

```cpp
class WhisperServer {
public:
    explicit WhisperServer(const ServerConfig& config);  // Constructor
    ~WhisperServer();                                     // Destructor

    bool init();
    void run();
    void stop();

private:
    ServerConfig config_;
    std::vector<std::unique_ptr<ContextSlot>> context_pool_;
    std::mutex context_pool_mutex_;
};
```

**public**: Anyone can access
**private**: Only the class itself can access

**Constructor**: Called when object is created
**Destructor** (`~`): Called when object is destroyed

**`explicit`**: Prevents accidental implicit conversions

```cpp
// Without explicit, this would compile (bad):
WhisperServer server = some_config;  // Implicit conversion

// With explicit, you must be clear:
WhisperServer server(some_config);   // OK
```

---

## Understanding the Implementation

Now let's read `whisper_server.cpp`.

### Static Functions

```cpp
static std::string generateSessionId() {
    // ...
}
```

`static` here means "only visible in this file." It's a helper function not exposed in the header.

### Random Number Generation

```cpp
static std::string generateSessionId() {
    static std::random_device rd;           // True random (for seeding)
    static std::mt19937 gen(rd());          // Mersenne Twister PRNG
    static std::uniform_int_distribution<> dis(0, 15);  // Range [0, 15]
    static const char* hex = "0123456789abcdef";

    std::string id;
    id.reserve(16);  // Pre-allocate space (optimization)
    for (int i = 0; i < 16; ++i) {
        id += hex[dis(gen)];  // Random hex character
    }
    return id;
}
```

`static` inside a function = "initialize once, persist across calls."

### Constructor Implementation

```cpp
WhisperServer::WhisperServer(const ServerConfig& config)
    : config_(config) {  // Initializer list
}
```

**Initializer list** (`: config_(config)`): Initialize members before constructor body runs. More efficient than assignment in the body.

### Destructor

```cpp
WhisperServer::~WhisperServer() {
    stop();  // Clean shutdown

    // Free all whisper contexts
    for (auto& slot : context_pool_) {
        if (slot && slot->ctx) {
            whisper_free(slot->ctx);  // C API cleanup
            slot->ctx = nullptr;
        }
    }
}
```

The destructor cleans up resources. Called automatically when object is destroyed.

### Range-Based For Loop

```cpp
for (auto& slot : context_pool_) {
    // slot is a reference to each element
}
```

`auto` = compiler figures out the type
`&` = reference (no copying, can modify original)

Equivalent to:
```cpp
for (int i = 0; i < context_pool_.size(); ++i) {
    auto& slot = context_pool_[i];
}
```

### std::make_unique

```cpp
auto slot = std::make_unique<ContextSlot>();
```

Creates a `unique_ptr<ContextSlot>`. Safer than `new` because:
- No raw `new`/`delete`
- Exception-safe
- Clear ownership

### Moving Objects

```cpp
context_pool_.push_back(std::move(slot));
```

`std::move` transfers ownership. After this, `slot` is empty/invalid. The `ContextSlot` now lives inside `context_pool_`.

### Lock Guards (RAII)

```cpp
{
    std::lock_guard<std::mutex> lock(context_pool_mutex_);
    // Mutex is locked here
    // ...
}  // Mutex automatically unlocked when 'lock' goes out of scope
```

RAII = "Resource Acquisition Is Initialization"
- Acquire resource in constructor
- Release in destructor
- Guarantees cleanup even if exceptions occur

### Structured Bindings (C++17)

```cpp
for (auto& [id, session] : sessions_) {
    // id = key (std::string)
    // session = value (std::shared_ptr<Session>)
}
```

Unpacks pairs/tuples into named variables. Like JavaScript destructuring.

### Lambda Functions

```cpp
session->send_message = [ws](const std::string& msg) {
    ws->send(msg, uWS::OpCode::TEXT);
};
```

Anatomy:
```cpp
[capture](parameters) -> return_type { body }
```

- `[ws]` = capture `ws` by value (copy the pointer)
- `[&ws]` = capture by reference
- `[=]` = capture everything by value
- `[&]` = capture everything by reference

### Casting

```cpp
const int16_t* data = reinterpret_cast<const int16_t*>(message.data());
```

`reinterpret_cast` tells the compiler "treat these bytes as this type." Dangerous if misused, but necessary for binary protocols.

### std::this_thread

```cpp
std::this_thread::sleep_for(std::chrono::milliseconds(10));
```

Sleeps the current thread. `std::chrono` is C++'s time library.

```cpp
using namespace std::chrono;
auto start = steady_clock::now();
// ... do work ...
auto elapsed = steady_clock::now() - start;
auto sleep_time = milliseconds(500) - elapsed;
```

### String Operations

```cpp
std::string text;
text += segment_text;  // Append

// Trim whitespace
size_t start = text.find_first_not_of(" \t\n\r");
size_t end = text.find_last_not_of(" \t\n\r");
text = text.substr(start, end - start + 1);
```

---

## The Main Entry Point

`main.cpp` is where execution starts.

### Command Line Arguments

```cpp
int main(int argc, char* argv[]) {
    // argc = argument count
    // argv = argument values (array of C strings)
    // argv[0] = program name
    // argv[1] = first argument, etc.
}
```

### Parsing Arguments

```cpp
for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--port" && i + 1 < argc) {
        config.port = std::stoi(argv[++i]);  // stoi = string to int
    }
}
```

### uWebSockets Setup

```cpp
uWS::App app;

app.ws<PerSocketData>("/*", {
    .open = [](auto* ws) { /* connection opened */ },
    .message = [](auto* ws, std::string_view msg, uWS::OpCode op) { /* data received */ },
    .close = [](auto* ws, int code, std::string_view msg) { /* connection closed */ }
});

app.listen(port, [](auto* listen_socket) {
    if (listen_socket) {
        std::cout << "Listening on port " << port << std::endl;
    }
});

app.run();  // Blocks, runs event loop
```

### Per-Socket Data

```cpp
struct PerSocketData {
    std::string session_id;
};
```

Each WebSocket connection can store custom data. Access via:
```cpp
auto* data = ws->getUserData();
data->session_id = "abc123";
```

---

## Memory Management

### The Three Rules of C++ Memory

1. **Stack allocation**: Automatic, fast, limited size
   ```cpp
   int x = 5;              // On stack
   std::string name;       // On stack (but string's buffer is on heap)
   ```

2. **Heap allocation**: Manual, unlimited size
   ```cpp
   int* p = new int(5);    // On heap
   delete p;               // Must delete!
   ```

3. **Smart pointers**: Automatic heap management
   ```cpp
   auto p = std::make_unique<int>(5);  // On heap, auto-deleted
   ```

### Why We Use unique_ptr for ContextSlot

```cpp
std::vector<std::unique_ptr<ContextSlot>> context_pool_;
```

Problem: `std::vector` may resize and copy/move elements. `std::atomic` can't be copied.

Solution: Store pointers. `unique_ptr` can be moved (just copying the address), and the `ContextSlot` itself stays put.

### Why We Use shared_ptr for Session

```cpp
std::shared_ptr<Session> session;
```

Sessions are accessed from:
1. The `sessions_` map
2. The inference loop
3. WebSocket handlers

`shared_ptr` lets multiple places hold references safely. The Session is deleted only when ALL references are gone.

---

## Threading and Synchronization

### Why Mutexes?

Without synchronization, two threads accessing the same data can corrupt it:

```
Thread A reads: x = 5
Thread B reads: x = 5
Thread A writes: x = 6
Thread B writes: x = 7  // Thread A's write lost!
```

### Mutex Pattern

```cpp
std::mutex mutex_;

void safeOperation() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only one thread can be here at a time
    // ... modify shared data ...
}  // Lock released automatically
```

### Our Threading Model

```
Main Thread              Inference Thread
-----------              ----------------
WebSocket events         while (running_) {
  ↓                        for each session:
acquireContext()             runInference()
sessions_ access           sleep(step_ms)
releaseContext()         }
```

Shared data protected by mutexes:
- `context_pool_mutex_` protects `context_pool_`
- `sessions_mutex_` protects `sessions_`

---

## Common Patterns

### RAII (Resource Acquisition Is Initialization)

```cpp
class FileHandle {
    FILE* file_;
public:
    FileHandle(const char* path) : file_(fopen(path, "r")) {}
    ~FileHandle() { if (file_) fclose(file_); }
};

{
    FileHandle f("data.txt");
    // use file...
}  // File automatically closed
```

We use this pattern everywhere:
- `lock_guard` for mutexes
- `unique_ptr` for memory
- Destructor cleanup in `WhisperServer`

### Factory Functions

```cpp
std::shared_ptr<Session> WhisperServer::createSession(...) {
    auto session = std::make_shared<Session>();
    // initialize...
    return session;
}
```

Functions that create and return objects. Clearer than raw `new`.

### Callbacks via std::function

```cpp
// Store callback
std::function<void(const std::string&)> send_message;

// Assign callback
send_message = [ws](const std::string& msg) {
    ws->send(msg, uWS::OpCode::TEXT);
};

// Invoke callback
send_message("Hello!");
```

Decouples "what to do" from "when to do it."

### const Correctness

```cpp
const std::string& text                    // Can't modify text
void foo(const ServerConfig& config)       // Can't modify config
bool isRunning() const { return running_; } // Method doesn't modify object
```

`const` = promise not to modify. Helps catch bugs and enables optimizations.

---

## Exercises

Try these to deepen your understanding:

1. **Add logging**: Print a message every time `runInference` is called with the session ID.

2. **Add a message counter**: Count how many audio chunks each session receives. Add `int chunk_count` to `Session`.

3. **Add a `/stats` HTTP endpoint**: uWebSockets supports HTTP too. Return JSON with session count and uptime.

4. **Implement VAD**: After `runInference`, if the text hasn't changed for 3 cycles, emit a "final" message.

---

## Further Reading

- [A Tour of C++](https://isocpp.org/tour) - Bjarne Stroustrup's intro
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/) - Best practices
- [cppreference.com](https://en.cppreference.com/) - Comprehensive reference
- [Compiler Explorer](https://godbolt.org/) - See what your code compiles to

---

## Quick Reference Card

```cpp
// Variables
int x = 5;                          // Integer
double y = 3.14;                    // Floating point
std::string s = "hello";            // String
std::vector<int> v = {1, 2, 3};     // Dynamic array
std::unordered_map<std::string, int> m;  // Hash map

// Pointers and References
int* ptr = &x;                      // Pointer to x
int& ref = x;                       // Reference to x
*ptr = 10;                          // Dereference pointer
ptr->member                         // Access member via pointer

// Smart Pointers
auto u = std::make_unique<Foo>();   // Unique ownership
auto s = std::make_shared<Foo>();   // Shared ownership
u->method();                        // Use like regular pointer

// Loops
for (int i = 0; i < 10; ++i) {}     // Classic for
for (auto& item : container) {}     // Range-based for
while (condition) {}                // While loop

// Functions
void foo(int x);                    // Declaration
void foo(int x) { /* body */ }      // Definition
auto bar = [](int x) { return x*2; };  // Lambda

// Classes
class Foo {
public:
    Foo();                          // Constructor
    ~Foo();                         // Destructor
    void method();                  // Member function
private:
    int member_;                    // Member variable
};

// Threading
std::thread t(function);            // Start thread
t.join();                           // Wait for thread
std::mutex m;                       // Mutex
std::lock_guard<std::mutex> lock(m); // RAII lock
std::atomic<bool> flag;             // Atomic variable
```

Happy coding! Feel free to experiment and break things - that's how you learn.
