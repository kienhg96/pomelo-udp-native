cmake_minimum_required(VERSION 3.10)

project(pomelo VERSION 0.0.1)
set(CMAKE_C_STANDARD 11)


# Options
option(POMELO_BUILD_TESTS "Build with tests" ON)
option(POMELO_BUILD_EXAMPLES "Build with examples" ON)
option(POMELO_BUILD_GENERATOR "Build generator" ON)


# Modules
set(POMELO_BASE pomelo-base)
set(POMELO_PROTOCOL pomelo-protocol)
set(POMELO_ADAPTER_DEFAULT pomelo-adapter-default)
set(POMELO_CRYPTO pomelo-crypto)
set(POMELO_UTILS pomelo-utils)
set(POMELO_DELIVERY pomelo-delivery)
set(POMELO_PLATFORM_UV pomelo-platform-uv)
set(POMELO_API pomelo-api)
set(POMELO_GENERATOR pomelo-generator)


# Include paths
set(POMELO_INCLUDE include src)

# libuv
# First check if libuv target is already available (e.g., from parent project)
if (TARGET uv_a)
    message(STATUS "Using libuv from parent project")
    set(LIB_UV uv_a)
    get_target_property(LIB_UV_INCLUDE uv_a INTERFACE_INCLUDE_DIRECTORIES)
elseif (TARGET uv)
    message(STATUS "Using libuv (shared) from parent project")
    set(LIB_UV uv)
    get_target_property(LIB_UV_INCLUDE uv INTERFACE_INCLUDE_DIRECTORIES)
else()
    message(STATUS "Using bundled libuv")
    set(LIB_UV uv_a)
    set(LIB_UV_PATH deps/libuv)
    set(LIB_UV_INCLUDE ${LIB_UV_PATH}/include)
    set(LIBUV_BUILD_SHARED OFF)
    add_subdirectory(${LIB_UV_PATH} EXCLUDE_FROM_ALL)
endif()


# sodium
set(LIB_SODIUM sodium)
set(LIB_SODIUM_PATH deps/sodium)
set(LIB_SODIUM_SRC_PATH deps/libsodium/src/libsodium)
set(LIB_SODIUM_INCLUDE
    ${LIB_SODIUM_SRC_PATH}/include
    ${LIB_SODIUM_SRC_PATH}/include/sodium
)
set(LIB_SODIUM_DEFINES SODIUM_STATIC=1 DEV_MODE=0 CONFIGURED=1)
add_subdirectory(${LIB_SODIUM_PATH})


# Sources
set(SRC_INCLUDE
    include/pomelo/address.h
    include/pomelo/allocator.h
    include/pomelo/api.h
    include/pomelo/base64.h
    include/pomelo/common.h
    include/pomelo/constants.h
    include/pomelo/errno.h
    include/pomelo/platform.h
    include/pomelo/plugin.h
    include/pomelo/random.h
    include/pomelo/statistic.h
    include/pomelo/statistic/statistic-allocator.h
    include/pomelo/statistic/statistic-api.h
    include/pomelo/statistic/statistic-buffer.h
    include/pomelo/statistic/statistic-delivery.h
    include/pomelo/statistic/statistic-protocol.h
    include/pomelo/token.h
    include/pomelo.h
)

set(SRC_API
    src/api/builtin/builtin.h
    src/api/builtin/channel.c
    src/api/builtin/channel.h
    src/api/builtin/delivery.c
    src/api/builtin/protocol.c
    src/api/builtin/session.c
    src/api/builtin/session.h
    src/api/plugin/channel.c
    src/api/plugin/channel.h
    src/api/plugin/executor.c
    src/api/plugin/executor.h
    src/api/plugin/loader.c
    src/api/plugin/manager.c
    src/api/plugin/manager.h
    src/api/plugin/message.c
    src/api/plugin/message.h
    src/api/plugin/plugin.c
    src/api/plugin/plugin.h
    src/api/plugin/session.c
    src/api/plugin/session.h
    src/api/plugin/socket.c
    src/api/plugin/socket.h
    src/api/plugin/token.c
    src/api/plugin/token.h
    src/api/channel.c
    src/api/channel.h
    src/api/context.c
    src/api/context.h
    src/api/message.c
    src/api/message.h
    src/api/session.c
    src/api/session.h
    src/api/socket.c
    src/api/socket.h
)

set(SRC_BASE
    ${SRC_INCLUDE}
    src/base/address.c
    src/base/allocator.c
    src/base/buffer.c
    src/base/buffer.h
    src/base/constants.h
    src/base/extra.h
    src/base/payload.c
    src/base/payload.h
    src/base/pipeline.c
    src/base/pipeline.h
    src/base/ref.c
    src/base/ref.h
    src/base/sequencer.c
    src/base/sequencer.h
)

set(SRC_CRYPTO
    src/crypto/base64.c
    src/crypto/checksum.c
    src/crypto/checksum.h
    src/crypto/crypto.c
    src/crypto/crypto.h
    src/crypto/random.c
    src/crypto/token.c
    src/crypto/token.h
)

set(SRC_DELIVERY
    src/delivery/bus.c
    src/delivery/bus.h
    src/delivery/clock.c
    src/delivery/clock.h
    src/delivery/context.c
    src/delivery/context.h
    src/delivery/delivery.h
    src/delivery/dispatcher.c
    src/delivery/dispatcher.h
    src/delivery/endpoint.c
    src/delivery/endpoint.h
    src/delivery/fragment.c
    src/delivery/fragment.h
    src/delivery/heartbeat.c
    src/delivery/heartbeat.h
    src/delivery/internal.h
    src/delivery/parcel.c
    src/delivery/parcel.h
    src/delivery/receiver.c
    src/delivery/receiver.h
    src/delivery/sender.c
    src/delivery/sender.h
)

set(SRC_GENERATOR
    src/generator/args.h
    src/generator/args.c
    src/generator/generator.c
)


set(SRC_PLATFORM_UV
    include/pomelo/platforms/platform-uv.h
    src/platform/uv/executor.c
    src/platform/uv/executor.h
    src/platform/uv/glue.c
    src/platform/uv/platform-uv.c
    src/platform/uv/platform-uv.h
    src/platform/uv/worker.c
    src/platform/uv/worker.h
    src/platform/uv/time.c
    src/platform/uv/timer.c
    src/platform/uv/timer.h
    src/platform/uv/udp.c
    src/platform/uv/udp.h
    src/platform/platform.h
)


set(SRC_PROTOCOL
    src/protocol/adapter.c
    src/protocol/client.c
    src/protocol/client.h
    src/protocol/context.c
    src/protocol/context.h
    src/protocol/crypto.c
    src/protocol/crypto.h
    src/protocol/emitter.c
    src/protocol/emitter.h
    src/protocol/packet.c
    src/protocol/packet.h
    src/protocol/peer.c
    src/protocol/peer.h
    src/protocol/protocol.h
    src/protocol/receiver.c
    src/protocol/receiver.h
    src/protocol/sender.c
    src/protocol/sender.h
    src/protocol/server.c
    src/protocol/server.h
    src/protocol/socket.c
    src/protocol/socket.h
)

set(SRC_ADAPTER_BASE src/adapter/adapter.h)
set(SRC_ADAPTER_DEFAULT src/adapter/default.c src/adapter/default.h)

set(SRC_UTILS
    src/utils/array.c
    src/utils/array.h
    src/utils/atomic.c
    src/utils/atomic.h
    src/utils/clz.c
    src/utils/clz.h
    src/utils/heap.c
    src/utils/heap.h
    src/utils/list.c
    src/utils/list.h
    src/utils/macro.h
    src/utils/map.c
    src/utils/map.h
    src/utils/mutex.c
    src/utils/mutex.h
    src/utils/pool.c
    src/utils/pool.h
    src/utils/rtt.c
    src/utils/rtt.h
    src/utils/sampling.c
    src/utils/sampling.h
)


# Set warning flags
if(MSVC)
    set(POMELO_COMPILE_FLAGS /W4 /WX /Wv:18)
    set(POMELO_LINK_FLAGS)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(POMELO_COMPILE_FLAGS -Wall -Wpedantic -Wextra -Werror)
    set(POMELO_LINK_FLAGS -ldl)
endif()


# Base
add_library(${POMELO_BASE} STATIC ${SRC_BASE})
target_include_directories(${POMELO_BASE} PRIVATE ${LIB_UV_INCLUDE} ${POMELO_INCLUDE})
target_compile_definitions(${POMELO_BASE} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_BASE} PRIVATE ${POMELO_COMPILE_FLAGS})


# Protocol
add_library(${POMELO_PROTOCOL} STATIC ${SRC_PROTOCOL})
target_include_directories(${POMELO_PROTOCOL} PRIVATE ${POMELO_INCLUDE})
target_compile_definitions(${POMELO_PROTOCOL} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_PROTOCOL} PRIVATE ${POMELO_COMPILE_FLAGS})


# Adapter default
add_library(${POMELO_ADAPTER_DEFAULT} STATIC ${SRC_ADAPTER_BASE} ${SRC_ADAPTER_DEFAULT})
target_include_directories(${POMELO_ADAPTER_DEFAULT} PRIVATE ${POMELO_INCLUDE})
target_compile_definitions(${POMELO_ADAPTER_DEFAULT} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_ADAPTER_DEFAULT} PRIVATE ${POMELO_COMPILE_FLAGS})


# Crypto
add_library(${POMELO_CRYPTO} STATIC ${SRC_CRYPTO})
target_compile_definitions(${POMELO_CRYPTO} PRIVATE ${POMELO_COMPILE_DEFINES})
target_include_directories(${POMELO_CRYPTO} PRIVATE ${POMELO_INCLUDE} ${LIB_SODIUM_INCLUDE})
target_compile_options(${POMELO_CRYPTO} PRIVATE ${POMELO_COMPILE_FLAGS})
target_compile_definitions(${POMELO_CRYPTO} PUBLIC ${LIB_SODIUM_DEFINES})


# Utils
add_library(${POMELO_UTILS} STATIC ${SRC_UTILS})
target_include_directories(${POMELO_UTILS} PRIVATE ${LIB_UV_INCLUDE} ${POMELO_INCLUDE})
target_compile_definitions(${POMELO_UTILS} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_UTILS} PRIVATE ${POMELO_COMPILE_FLAGS})


# Delivery
add_library(${POMELO_DELIVERY} STATIC ${SRC_DELIVERY})
target_include_directories(${POMELO_DELIVERY} PRIVATE ${POMELO_INCLUDE})
target_compile_definitions(${POMELO_DELIVERY} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_DELIVERY} PRIVATE ${POMELO_COMPILE_FLAGS})


# Platform UV
add_library(${POMELO_PLATFORM_UV} STATIC ${SRC_PLATFORM_UV})
target_include_directories(${POMELO_PLATFORM_UV}
    PUBLIC ${LIB_UV_INCLUDE}
    PRIVATE ${POMELO_INCLUDE}
)
target_compile_definitions(${POMELO_PLATFORM_UV} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_PLATFORM_UV} PRIVATE ${POMELO_COMPILE_FLAGS})
if (WIN32)
    target_compile_definitions(${POMELO_PLATFORM_UV} PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0602 # Windows 8
    )
endif()


# API
add_library(${POMELO_API} STATIC ${SRC_API})
target_include_directories(${POMELO_API} PUBLIC ${POMELO_INCLUDE})
target_compile_definitions(${POMELO_API} PRIVATE ${POMELO_COMPILE_DEFINES})
target_compile_options(${POMELO_API} PRIVATE ${POMELO_COMPILE_FLAGS})


# Generator
if (POMELO_BUILD_GENERATOR)
    add_executable(${POMELO_GENERATOR} ${SRC_GENERATOR})
    target_include_directories(${POMELO_GENERATOR} PRIVATE ${POMELO_INCLUDE} ${LIB_SODIUM_INCLUDE})
    target_compile_definitions(${POMELO_GENERATOR} PRIVATE ${POMELO_COMPILE_DEFINES})
    target_link_libraries(${POMELO_GENERATOR} PRIVATE
        ${POMELO_BASE}
        ${POMELO_CRYPTO}
        ${POMELO_UTILS}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_GENERATOR} PRIVATE ${POMELO_COMPILE_FLAGS})
    target_compile_definitions(${POMELO_GENERATOR} PUBLIC ${LIB_SODIUM_DEFINES})
endif()


# Tests
if (POMELO_BUILD_TESTS)
    set(POMELO_TEST_BASE pomelo-test-base)
    set(POMELO_TEST_CRYPTO pomelo-test-crypto)
    set(POMELO_TEST_PLATFORM_UV pomelo-test-platform-uv)
    set(POMELO_TEST_PROTOCOL pomelo-test-protocol)
    set(POMELO_TEST_PROTOCOL_UNENCRYPTED pomelo-test-protocol-unencrypted)
    set(POMELO_TEST_PROTOCOL_CLIENT pomelo-test-protocol-client)
    set(POMELO_TEST_PROTOCOL_SERVER pomelo-test-protocol-server)
    set(POMELO_TEST_PROTOCOL_SERVER_DOS pomelo-test-protocol-server-dos)
    set(POMELO_TEST_PROTOCOL_PACKET pomelo-test-protocol-packet)
    set(POMELO_TEST_DELIVERY_SINGLE pomelo-test-delivery-single)
    set(POMELO_TEST_DELIVERY_MALFORMED pomelo-test-delivery-malformed)
    set(POMELO_TEST_DELIVERY_MULTIPLE pomelo-test-delivery-multiple)
    set(POMELO_TEST_API_BASIC pomelo-test-api-basic)
    set(POMELO_TEST_API_BROADCAST pomelo-test-api-broadcast)
    set(POMELO_TEST_PLUGIN pomelo-test-plugin)
    set(POMELO_TEST_DEMO_PLUGIN pomelo-test-demo-plugin)
    set(POMELO_TEST_WEBRTC_PLUGIN pomelo-test-webrtc-plugin)
    set(POMELO_TEST_UTILS pomelo-test-utils)
    set(POMELO_TEST_STATISTIC_CHECK pomelo-test-statistic-check)

    set(POMELO_TEST_INCLUDE ${POMELO_INCLUDE} test)

    # Test statistic check
    set(SRC_TEST_STATISTIC_CHECK
        test/statistic-check/statistic-check.c
        test/statistic-check/statistic-check.h
    )
    add_library(${POMELO_TEST_STATISTIC_CHECK} STATIC ${SRC_TEST_STATISTIC_CHECK})
    target_include_directories(${POMELO_TEST_STATISTIC_CHECK} PRIVATE ${POMELO_TEST_INCLUDE})


    # Test base
    set(SRC_TEST_BASE
        test/base-test/address-test.c
        test/base-test/allocator-test.c
        test/base-test/base-test.c
        test/base-test/payload-test.c
        test/base-test/ref-test.c
    )

    add_executable(${POMELO_TEST_BASE} ${SRC_TEST_BASE})
    target_include_directories(${POMELO_TEST_BASE} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_BASE} PRIVATE
        ${POMELO_BASE}
        ${POMELO_UTILS}
        ${POMELO_CRYPTO}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_BASE} PRIVATE ${POMELO_COMPILE_FLAGS})

    # Test crypto
    set(SRC_TEST_CRYPTO test/crypto-test/crypto-test.c)
    add_executable(${POMELO_TEST_CRYPTO} ${SRC_TEST_CRYPTO})
    target_include_directories(${POMELO_TEST_CRYPTO} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_CRYPTO} PRIVATE
        ${POMELO_BASE}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_TEST_CRYPTO} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test utils
    set(SRC_TEST_UTILS
        test/utils-test/utils-test.c
        test/utils-test/utils-test.h
        test/utils-test/array-test.c
        test/utils-test/heap-test.c
        test/utils-test/list-test.c
        test/utils-test/map-test.c
        test/utils-test/pool-test.c
    )
    add_executable(${POMELO_TEST_UTILS} ${SRC_TEST_UTILS})
    target_include_directories(${POMELO_TEST_UTILS} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_UTILS} PRIVATE
        ${POMELO_BASE}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
    )
    target_compile_options(${POMELO_TEST_UTILS} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test Platform UV
    set(SRC_TEST_PLATFORM_UV
        test/platform-test/platform-test.c
        test/platform-test/platform-test.h
        test/platform-test/platform-uv.c
    )
    add_executable(${POMELO_TEST_PLATFORM_UV} ${SRC_TEST_PLATFORM_UV})
    target_include_directories(${POMELO_TEST_PLATFORM_UV} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PLATFORM_UV} PRIVATE
        ${POMELO_UTILS}
        ${POMELO_BASE}
        ${POMELO_PLATFORM_UV}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PLATFORM_UV} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test protocol: Client
    set(SRC_TEST_PROTOCOL_CLIENT
        ${SRC_ADAPTER_BASE}
        test/protocol-test/adapter-simulator.c
        test/protocol-test/adapter-simulator.h
        test/protocol-test/client-test.c
    )
    add_executable(${POMELO_TEST_PROTOCOL_CLIENT} ${SRC_TEST_PROTOCOL_CLIENT})
    target_include_directories(${POMELO_TEST_PROTOCOL_CLIENT} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PROTOCOL_CLIENT} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PROTOCOL_CLIENT} PRIVATE ${POMELO_COMPILE_FLAGS})
    target_compile_definitions(${POMELO_TEST_PROTOCOL_CLIENT} PRIVATE)


    # Test protocol: Server
    set(SRC_TEST_PROTOCOL_SERVER
        ${SRC_ADAPTER_BASE}
        test/protocol-test/adapter-simulator.c
        test/protocol-test/adapter-simulator.h
        test/protocol-test/server-test.c
    )
    add_executable(${POMELO_TEST_PROTOCOL_SERVER} ${SRC_TEST_PROTOCOL_SERVER})
    target_include_directories(${POMELO_TEST_PROTOCOL_SERVER} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PROTOCOL_SERVER} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PROTOCOL_SERVER} PRIVATE ${POMELO_COMPILE_FLAGS})
    target_compile_definitions(${POMELO_TEST_PROTOCOL_SERVER} PRIVATE)


    # Test protocol: Server anti-DoS (half-open peer cap)
    set(SRC_TEST_PROTOCOL_SERVER_DOS
        ${SRC_ADAPTER_BASE}
        test/protocol-test/adapter-simulator.c
        test/protocol-test/adapter-simulator.h
        test/protocol-test/server-dos-test.c
    )
    add_executable(${POMELO_TEST_PROTOCOL_SERVER_DOS} ${SRC_TEST_PROTOCOL_SERVER_DOS})
    target_include_directories(${POMELO_TEST_PROTOCOL_SERVER_DOS} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PROTOCOL_SERVER_DOS} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PROTOCOL_SERVER_DOS} PRIVATE ${POMELO_COMPILE_FLAGS})
    target_compile_definitions(${POMELO_TEST_PROTOCOL_SERVER_DOS} PRIVATE)


    # Test protocol: Packet
    set(SRC_TEST_PROTOCOL_PACKET test/protocol-test/packet-test.c)
    add_executable(${POMELO_TEST_PROTOCOL_PACKET} ${SRC_TEST_PROTOCOL_PACKET})
    target_include_directories(${POMELO_TEST_PROTOCOL_PACKET} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PROTOCOL_PACKET} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_CRYPTO}
        ${POMELO_PLATFORM_UV}
        ${POMELO_ADAPTER_DEFAULT}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PROTOCOL_PACKET} PRIVATE ${POMELO_COMPILE_FLAGS})
    target_compile_definitions(${POMELO_TEST_PROTOCOL_PACKET} PRIVATE)


    # Test protocol: Encrypted packets tranfering
    set(SRC_TEST_PROTOCOL test/protocol-test/protocol-test.c)
    add_executable(${POMELO_TEST_PROTOCOL} ${SRC_TEST_PROTOCOL})
    target_include_directories(${POMELO_TEST_PROTOCOL} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PROTOCOL} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_ADAPTER_DEFAULT}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PROTOCOL} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test protocol: Unencrypted packets tranfering
    set(SRC_TEST_PROTOCOL_UNENCRYPTED
        ${SRC_TEST_PROTOCOL}
        ${SRC_ADAPTER_BASE}
        ${SRC_ADAPTER_DEFAULT}
    )
    add_executable(${POMELO_TEST_PROTOCOL_UNENCRYPTED} ${SRC_TEST_PROTOCOL_UNENCRYPTED})
    target_include_directories(${POMELO_TEST_PROTOCOL_UNENCRYPTED} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PROTOCOL_UNENCRYPTED} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_compile_options(${POMELO_TEST_PROTOCOL_UNENCRYPTED} PRIVATE ${POMELO_COMPILE_FLAGS})
    target_compile_definitions(${POMELO_TEST_PROTOCOL_UNENCRYPTED} PRIVATE POMELO_ADAPTER_DEFAULT_NO_ENCRYPTION)


    # Test delivery: Single
    set(SRC_TEST_DELIVERY_SINGLE test/delivery-test/delivery-test-single.c)
    add_executable(${POMELO_TEST_DELIVERY_SINGLE} ${SRC_TEST_DELIVERY_SINGLE})
    target_include_directories(${POMELO_TEST_DELIVERY_SINGLE} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_DELIVERY_SINGLE} PRIVATE
        ${POMELO_BASE}
        ${POMELO_UTILS}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_PLATFORM_UV}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_TEST_DELIVERY_SINGLE} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test delivery: Malformed fragment guards
    set(SRC_TEST_DELIVERY_MALFORMED test/delivery-test/delivery-malformed-test.c)
    add_executable(${POMELO_TEST_DELIVERY_MALFORMED} ${SRC_TEST_DELIVERY_MALFORMED})
    target_include_directories(${POMELO_TEST_DELIVERY_MALFORMED} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_DELIVERY_MALFORMED} PRIVATE
        ${POMELO_BASE}
        ${POMELO_UTILS}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_PLATFORM_UV}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_TEST_DELIVERY_MALFORMED} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test delivery: Multiple
    set(SRC_TEST_DELIVERY_MULTIPLE test/delivery-test/delivery-test-multiple.c)
    add_executable(${POMELO_TEST_DELIVERY_MULTIPLE} ${SRC_TEST_DELIVERY_MULTIPLE})
    target_include_directories(${POMELO_TEST_DELIVERY_MULTIPLE} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_DELIVERY_MULTIPLE} PRIVATE
        ${POMELO_BASE}
        ${POMELO_UTILS}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_PLATFORM_UV}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_TEST_DELIVERY_MULTIPLE} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test API: Basic
    set(SRC_TEST_API_BASIC
        test/api-test/api-basic-test.c
        test/platform-test/platform-uv.c
    )
    add_executable(${POMELO_TEST_API_BASIC} ${SRC_TEST_API_BASIC})
    target_include_directories(${POMELO_TEST_API_BASIC} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_API_BASIC} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_API}
        ${POMELO_ADAPTER_DEFAULT}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_TEST_API_BASIC} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test API: Broadcast
    set(SRC_TEST_API_BROADCAST
        test/api-test/api-broadcast-test.c
        test/platform-test/platform-uv.c
    )
    add_executable(${POMELO_TEST_API_BROADCAST} ${SRC_TEST_API_BROADCAST})
    target_include_directories(${POMELO_TEST_API_BROADCAST} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_API_BROADCAST} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_API}
        ${POMELO_ADAPTER_DEFAULT}
        ${POMELO_TEST_STATISTIC_CHECK}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_compile_options(${POMELO_TEST_API_BROADCAST} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Demo plugin
    set(SRC_TEST_DEMO_PLUGIN test/plugin-test/demo-plugin.c)
    add_library(${POMELO_TEST_DEMO_PLUGIN} SHARED ${SRC_TEST_DEMO_PLUGIN})
    target_include_directories(${POMELO_TEST_DEMO_PLUGIN} PRIVATE include)


    # Test plugin
    set(SRC_TEST_PLUGIN test/plugin-test/plugin-test.c)
    add_executable(${POMELO_TEST_PLUGIN} ${SRC_TEST_PLUGIN})
    target_include_directories(${POMELO_TEST_PLUGIN} PRIVATE ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_PLUGIN} PRIVATE
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_API}
        ${POMELO_ADAPTER_DEFAULT}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_link_options(${POMELO_TEST_PLUGIN} PRIVATE ${POMELO_LINK_FLAGS})
    add_dependencies(${POMELO_TEST_PLUGIN} ${POMELO_TEST_DEMO_PLUGIN})
    target_compile_options(${POMELO_TEST_PLUGIN} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Test WebRTC plugin
    set(SRC_TEST_WEBRTC_PLUGIN test/webrtc-plugin-test/webrtc-plugin-test.c)
    add_executable(${POMELO_TEST_WEBRTC_PLUGIN} ${SRC_TEST_WEBRTC_PLUGIN})
    target_include_directories(${POMELO_TEST_WEBRTC_PLUGIN} PUBLIC ${POMELO_TEST_INCLUDE})
    target_link_libraries(${POMELO_TEST_WEBRTC_PLUGIN} PRIVATE 
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_API}
        ${POMELO_ADAPTER_DEFAULT}
        ${LIB_SODIUM}
        ${LIB_UV}
    )
    target_link_options(${POMELO_TEST_WEBRTC_PLUGIN} PRIVATE ${POMELO_LINK_FLAGS})
    target_compile_options(${POMELO_TEST_WEBRTC_PLUGIN} PRIVATE ${POMELO_COMPILE_FLAGS})

    # Test cases
    enable_testing()

    set(CTEST_TEST_TIMEOUT 10)

    add_test(NAME ${POMELO_TEST_BASE} COMMAND ${POMELO_TEST_BASE})
    add_test(NAME ${POMELO_TEST_UTILS} COMMAND ${POMELO_TEST_UTILS})
    add_test(NAME ${POMELO_TEST_CRYPTO} COMMAND ${POMELO_TEST_CRYPTO})
    add_test(NAME ${POMELO_TEST_PLATFORM_UV} COMMAND ${POMELO_TEST_PLATFORM_UV})

    add_test(NAME ${POMELO_TEST_PROTOCOL} COMMAND ${POMELO_TEST_PROTOCOL})
    add_test(NAME ${POMELO_TEST_PROTOCOL_SERVER} COMMAND ${POMELO_TEST_PROTOCOL_SERVER})
    add_test(NAME ${POMELO_TEST_PROTOCOL_SERVER_DOS} COMMAND ${POMELO_TEST_PROTOCOL_SERVER_DOS})
    add_test(NAME ${POMELO_TEST_PROTOCOL_CLIENT} COMMAND ${POMELO_TEST_PROTOCOL_CLIENT})
    add_test(NAME ${POMELO_TEST_PROTOCOL_PACKET} COMMAND ${POMELO_TEST_PROTOCOL_PACKET})
    add_test(NAME ${POMELO_TEST_PROTOCOL_UNENCRYPTED} COMMAND ${POMELO_TEST_PROTOCOL_UNENCRYPTED})

    add_test(NAME ${POMELO_TEST_DELIVERY_SINGLE} COMMAND ${POMELO_TEST_DELIVERY_SINGLE})
    add_test(NAME ${POMELO_TEST_DELIVERY_MALFORMED} COMMAND ${POMELO_TEST_DELIVERY_MALFORMED})
    add_test(NAME ${POMELO_TEST_DELIVERY_MULTIPLE} COMMAND ${POMELO_TEST_DELIVERY_MULTIPLE})

    add_test(NAME ${POMELO_TEST_API_BASIC} COMMAND ${POMELO_TEST_API_BASIC})
    add_test(NAME ${POMELO_TEST_API_BROADCAST} COMMAND ${POMELO_TEST_API_BROADCAST})

    add_test(NAME ${POMELO_TEST_PLUGIN} COMMAND ${POMELO_TEST_PLUGIN})

endif() # POMELO_BUILD_TESTS


# Examples
if (POMELO_BUILD_EXAMPLES)
    set(POMELO_EXAMPLE_CLIENT pomelo-example-client)
    set(POMELO_EXAMPLE_SERVER pomelo-example-server)
    set(POMELO_EXAMPLE_SHARED pomelo-example-shared)

    # Example shared
    add_library(${POMELO_EXAMPLE_SHARED} example/shared.h example/shared.c)
    target_link_libraries(${POMELO_EXAMPLE_SHARED} PUBLIC
        ${POMELO_BASE}
        ${POMELO_PROTOCOL}
        ${POMELO_UTILS}
        ${POMELO_PLATFORM_UV}
        ${POMELO_CRYPTO}
        ${POMELO_DELIVERY}
        ${POMELO_API}
        ${POMELO_ADAPTER_DEFAULT}
        ${LIB_UV}
        ${LIB_SODIUM}
    )
    target_link_options(${POMELO_EXAMPLE_SHARED} PRIVATE ${POMELO_LINK_FLAGS})
    target_compile_options(${POMELO_EXAMPLE_SHARED} PRIVATE ${POMELO_COMPILE_FLAGS})

    # Example client
    add_executable(${POMELO_EXAMPLE_CLIENT} example/client.c)
    target_link_libraries(${POMELO_EXAMPLE_CLIENT} PRIVATE ${POMELO_EXAMPLE_SHARED})
    target_link_options(${POMELO_EXAMPLE_CLIENT} PRIVATE ${POMELO_LINK_FLAGS})
    target_compile_options(${POMELO_EXAMPLE_CLIENT} PRIVATE ${POMELO_COMPILE_FLAGS})


    # Example server
    add_executable(${POMELO_EXAMPLE_SERVER} example/server.c)
    target_link_libraries(${POMELO_EXAMPLE_SERVER} PRIVATE ${POMELO_EXAMPLE_SHARED})
    target_link_options(${POMELO_EXAMPLE_SERVER} PRIVATE ${POMELO_LINK_FLAGS})
    target_compile_options(${POMELO_EXAMPLE_SERVER} PRIVATE ${POMELO_COMPILE_FLAGS})
endif() # POMELO_BUILD_EXAMPLES
