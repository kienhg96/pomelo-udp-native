# Connect Token Documentation

## Overview
The connect token is a critical security component in the Pomelo protocol that enables secure client-server connections. It consists of two main parts: a private token (encrypted) and a public token.

## Token Structure

### 1. Private Token
The private portion is encrypted and signed with a private key shared between the web backend and dedicated server instances.

#### Structure Definition
```c
struct private_connect_token {
    int64_t client_id;              // Unique client identifier
    int32_t timeout_seconds;        // Connection timeout
    uint32_t num_server_addresses;  // Range: [1,32]
    address_data server_addresses[];// Server address list
    uint8_t client_to_server_key[32];// Encryption key for client→server
    uint8_t server_to_client_key[32];// Encryption key for server→client
    uint8_t user_data[256];        // Protocol-specific data
    uint8_t padding[];             // Zero pad to 1024 bytes
};
```

#### Field Details
- `client_id`: Globally unique identifier for an authenticated client
- `timeout_seconds`: Timeout in seconds (negative values disable timeout - development only)
- `num_server_addresses`: Number of server addresses (must be between 1 and 32)
- `server_addresses`: List of server addresses the client can connect to
- `client_to_server_key`: Key used for encrypting packets from client to server
- `server_to_client_key`: Key used for encrypting packets from server to client
- `user_data`: Application-specific data
- `padding`: Ensures fixed size and enhances encryption security

### 2. Public Token
The public token contains both encrypted and unencrypted data.

#### Structure Definition
```c
struct connect_token {
    char version_info[13];         // "POMELO 1.00\0"
    uint64_t protocol_id;          // Application identifier
    uint64_t create_timestamp;     // Token creation time
    uint64_t expire_timestamp;     // Token expiration time
    uint8_t nonce[24];            // Encryption nonce
    uint8_t private_data[1024];    // Encrypted private token
    int32_t timeout_seconds;       // Connection timeout
    uint32_t num_server_addresses; // Number of server addresses
    address_data server_addresses[];// Server address list
    uint8_t client_to_server_key[32];// Encryption key for client→server
    uint8_t server_to_client_key[32];// Encryption key for server→client
    uint8_t padding[];             // Zero pad to 2048 bytes
};
```

### 3. Address Data Structure
The `address_data` structure is used in both private and public tokens to represent network addresses.

#### Structure Definition
```c
struct address_data {
    uint8_t type;                  // Address type (IPv4 = 1, IPv6 = 2)
    union {
        struct {
            uint8_t ip[4];         // IPv4 address bytes
            uint16_t port;         // Port number
        } ipv4;
        struct {
            uint8_t ip[16];        // IPv6 address bytes
            uint16_t port;         // Port number
        } ipv6;
    };
};
```

#### Field Details
- `type`: Specifies the address format
  - `1`: IPv4 address
  - `2`: IPv6 address
- IPv4 Structure:
  - `ip`: Four bytes representing IPv4 address (network byte order)
  - `port`: Two bytes for port number (network byte order)
- IPv6 Structure:
  - `ip`: Sixteen bytes representing IPv6 address (network byte order)
  - `port`: Two bytes for port number (network byte order)

#### Usage in Connect Token
- Both private and public tokens can contain up to 32 server addresses
- All addresses must be valid and properly formatted
- Port numbers must be in network byte order
- IPv4 and IPv6 addresses can be mixed in the same token

## Encryption Process

### 1. Private Token Encryption
The private token is encrypted using libsodium's AEAD primitive:
```c
crypto_aead_xchacha20poly1305_ietf_encrypt
```

#### Associated Data
```c
struct {
    char version_info[13];     // "POMELO 1.00\0"
    uint64_t protocol_id;      // Application identifier
    uint64_t expire_timestamp; // Token expiration time
};
```

#### Encryption Details
- Nonce: 24 random bytes generated for each token
- Encryption Buffer: First 1008 bytes
- HMAC: Last 16 bytes
- Total Size: 1024 bytes
