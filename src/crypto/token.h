#ifndef POMELO_CODEC_TOKEN_SRC_H
#define POMELO_CODEC_TOKEN_SRC_H
#include "pomelo/constants.h"
#include "pomelo/token.h"
#include "base/payload.h"

#ifdef __cplusplus
extern "C" {
#endif


/// @brief The challenge token
typedef struct pomelo_challenge_token_s pomelo_challenge_token_t;


struct pomelo_challenge_token_s {
    /// @brief The client ID
    int64_t client_id;

    /// @brief The user data
    uint8_t user_data[POMELO_USER_DATA_BYTES];
};


/// @brief Encode & encrypt the token private part
/// The nonce has to be set before involking this function.
int pomelo_codec_encode_private_connect_token(
    uint8_t * buffer,
    pomelo_connect_token_t * token,
    const uint8_t * key
);


/// @brief Encode the array of server addresses
void pomelo_codec_encode_server_addresses(
    pomelo_payload_t * payload,
    pomelo_connect_token_t * info
);


/// @brief Decode the server address invidually
void pomelo_codec_decode_server_address(
    pomelo_payload_t * payload,
    pomelo_address_t * address
);


/// @brief Decode the array of server addresses
int pomelo_codec_decode_server_addresses(
    pomelo_payload_t * payload,
    int * naddresses,
    pomelo_address_t * addresses
);


/// @brief Encode the associated data for token private part
void pomelo_codec_encode_connect_token_associated_data(
    uint8_t * associated_data,
    pomelo_connect_token_t * info
);


/// @brief Encode the challenge token into payload
int pomelo_codec_encrypt_challenge_token(
    pomelo_payload_t * payload,
    pomelo_challenge_token_t * token,
    const uint8_t * key,
    uint64_t token_sequence // nonce
);


/// @brief Decode the challenge token from payload
int pomelo_codec_decrypt_challenge_token(
    pomelo_payload_t * payload,
    pomelo_challenge_token_t * token,
    const uint8_t * key,
    uint64_t token_sequence // nonce
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_CODEC_TOKEN_SRC_H
