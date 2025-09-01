# Pomelo Platform Layer Documentation

## Overview
The platform layer provides abstraction for system-level operations including network I/O, threading, and time management. It includes a common interface and a libuv-based implementation.

## Common Platform Interface

### Core Concepts
The platform layer provides:
- Network socket management
- Task scheduling and execution
- Timer management
- System time access
- Thread-safe execution

### Platform Structure
```c
typedef struct pomelo_platform_s pomelo_platform_t;
typedef struct pomelo_platform_task_s pomelo_platform_task_t;
typedef struct pomelo_threadsafe_executor_s pomelo_threadsafe_executor_t;
```

### Platform Lifecycle
```c
// Set/get platform extra data
void pomelo_platform_set_extra(pomelo_platform_t* platform, void* data);
void* pomelo_platform_get_extra(pomelo_platform_t* platform);
```

### Time Management
```c
// Get high-resolution time in nanoseconds (thread-safe)
uint64_t pomelo_platform_hrtime(pomelo_platform_t* platform);

// Get current time in milliseconds (thread-safe)
uint64_t pomelo_platform_now(pomelo_platform_t* platform);
```

### Thread-Safe Execution
```c
// Acquire a thread-safe executor
pomelo_threadsafe_executor_t* pomelo_platform_acquire_threadsafe_executor(
    pomelo_platform_t* platform
);

// Release a thread-safe executor
void pomelo_platform_release_threadsafe_executor(
    pomelo_platform_t* platform,
    pomelo_threadsafe_executor_t* executor
);

// Submit task to run in platform thread
pomelo_platform_task_t* pomelo_threadsafe_executor_submit(
    pomelo_threadsafe_executor_t* executor,
    pomelo_platform_task_entry entry,
    void* data
);
```

## libuv Platform Implementation

### Overview
The libuv implementation provides:
- Event-driven I/O
- Cross-platform compatibility
- High-performance networking
- Efficient timer management

### Configuration
```c

// The options for platform
typedef struct pomelo_platform_uv_options_s pomelo_platform_uv_options_t;


struct pomelo_platform_uv_options_s {
    // The allocator
    pomelo_allocator_t * allocator;

    // The UV loop
    uv_loop_t * uv_loop;
};


// Set the default options for platform
void pomelo_platform_uv_options_init(pomelo_platform_uv_options_t * options);

// Create the platform
pomelo_platform_t * pomelo_platform_uv_create(
    pomelo_platform_uv_options_t * options
);
```

### Features

#### Event Loop Integration
- Uses libuv event loop for I/O operations
- Supports existing loop integration
- Optional loop ownership

#### Network I/O
- UDP socket management
- Asynchronous I/O operations
- Buffer management
- Address handling

#### Timer Management
- High-resolution timers
- Periodic timer support
- Timer cancellation
- Efficient scheduling

#### Thread Pool
- Task execution in thread pool
- Work distribution
- Task cancellation
- Resource management

## Best Practices

### Platform Usage
1. Initialize platform before use
```c
// Initialize creating options
pomelo_platform_uv_options_t options;
pomelo_platform_uv_options_init(&options);

// Create new instance of libuv platform
pomelo_platform_t* platform = pomelo_platform_uv_create(&options);

```

2. Use thread-safe executors for cross-thread operations
```c
pomelo_threadsafe_executor_t* executor = 
    pomelo_platform_acquire_threadsafe_executor(platform);

// Submit task to run on platform thread
pomelo_threadsafe_executor_submit(executor, task_entry, task_data);
```

### Thread Safety Considerations

#### Thread-Safe Operations
- Time queries (hrtime, now)
- Executor task submission
- Extra data access

#### Main Thread Only
- Executor acquisition/release
- Timer management

### Error Handling
1. Check platform creation result
2. Validate executor acquisition
3. Handle task submission failures
4. Monitor timer operations

### Performance Optimization
1. Reuse executors when possible
2. Batch task submissions
3. Use appropriate timer intervals
4. Monitor thread pool usage

## Integration Example

### Basic Platform Setup
```c
// Initialize UV platform
pomelo_platform_uv_config_t config = {0};
config.loop = uv_default_loop();
config.own_loop = false;

pomelo_platform_t* platform = pomelo_platform_uv_create(&config);
if (!platform) {
    // Handle error
    return -1;
}

// Create thread-safe executor
pomelo_threadsafe_executor_t* executor = 
    pomelo_platform_acquire_threadsafe_executor(platform);

// Use platform...

// Cleanup
pomelo_platform_release_threadsafe_executor(platform, executor);
```

### Task Execution
```c
static void task_entry(void* data) {
    // Task implementation
}

// Submit task to platform thread
pomelo_threadsafe_executor_t* executor = 
    pomelo_platform_acquire_threadsafe_executor(platform);

pomelo_platform_task_t* task = 
    pomelo_threadsafe_executor_submit(executor, task_entry, data);

// Task will execute on platform thread
```

### Time Management
```c
// Get high-resolution time
uint64_t now_ns = pomelo_platform_hrtime(platform);

// Get current time
uint64_t now_ms = pomelo_platform_now(platform);
```
