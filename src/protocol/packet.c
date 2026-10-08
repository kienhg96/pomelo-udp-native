#include <assert.h>
#include <string.h>
#include "packet.h"


int pomelo_protocol_packet_request_init(
    pomelo_protocol_packet_request_t * packet,
    pomelo_protocol_packet_request_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_REQUEST;
    if (!info) return 0;

    packet->protocol_id = info->protocol_id;
    packet->expire_timestamp = info->expire_timestamp;

    if (info->connect_token_nonce) {
        memcpy(
            packet->connect_token_nonce,
            info->connect_token_nonce,
            POMELO_CONNECT_TOKEN_NONCE_BYTES
        );
    }

    if (info->encrypted_connect_token) {
        memcpy(
            packet->token_data.encrypted,
            info->encrypted_connect_token,
            POMELO_CONNECT_TOKEN_PRIVATE_BYTES
        );
    }

    return 0;
}


void pomelo_protocol_packet_request_cleanup(
    pomelo_protocol_packet_request_t * packet
) {
    (void) packet;
}


int pomelo_protocol_packet_denied_init(
    pomelo_protocol_packet_denied_t * packet,
    pomelo_protocol_packet_denied_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_DENIED;
    if (!info) return 0;

    packet->base.sequence = info->sequence;
    return 0;
}


void pomelo_protocol_packet_denied_cleanup(
    pomelo_protocol_packet_denied_t * packet
) {
    (void) packet;
}


int pomelo_protocol_packet_challenge_init(
    pomelo_protocol_packet_challenge_t * packet,
    pomelo_protocol_packet_challenge_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_CHALLENGE;
    if (!info) return 0;

    packet->base.sequence = info->sequence;
    packet->token_sequence = info->token_sequence;
    packet->challenge_data.token.client_id = info->client_id;

    if (info->user_data) {
        memcpy(
            packet->challenge_data.token.user_data,
            info->user_data,
            POMELO_USER_DATA_BYTES
        );
    }

    return 0;
}


void pomelo_protocol_packet_challenge_cleanup(
    pomelo_protocol_packet_challenge_t * packet
) {
    (void) packet;
}


int pomelo_protocol_packet_response_init(
    pomelo_protocol_packet_response_t * packet,
    pomelo_protocol_packet_response_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_RESPONSE;
    if (!info) return 0;

    packet->base.sequence = info->sequence;
    packet->token_sequence = info->token_sequence;
    memcpy(
        packet->challenge_data.encrypted,
        info->encrypted_challenge_token,
        POMELO_CHALLENGE_TOKEN_BYTES
    );
    return 0;
}


void pomelo_protocol_packet_response_cleanup(
    pomelo_protocol_packet_response_t * packet
) {
    (void) packet;
}


int pomelo_protocol_packet_payload_init(
    pomelo_protocol_packet_payload_t * packet,
    pomelo_protocol_packet_payload_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_PAYLOAD;
    if (!info) return 0;

    packet->base.sequence = info->sequence;
    pomelo_protocol_packet_payload_attach_views(
        packet,
        info->views,
        info->nviews
    );
    return 0;
}


void pomelo_protocol_packet_payload_cleanup(
    pomelo_protocol_packet_payload_t * packet
) {
    assert(packet != NULL);
    // Unref all buffers
    for (size_t i = 0; i < packet->nviews; i++) {
        pomelo_buffer_unref(packet->views[i].buffer);
        memset(&packet->views[i], 0, sizeof(pomelo_buffer_view_t));
    }
    packet->nviews = 0;
}


void pomelo_protocol_packet_payload_attach_views(
    pomelo_protocol_packet_payload_t * packet,
    pomelo_buffer_view_t * views,
    size_t nviews
) {
    assert(packet != NULL);
    assert(nviews == 0 || views != NULL);

    packet->nviews = nviews;
    if (nviews > 0) {
        memcpy(
            packet->views,
            views,
            nviews * sizeof(pomelo_buffer_view_t)
        );

        for (size_t i = 0; i < nviews; i++) {
            pomelo_buffer_ref(views[i].buffer);
        }
    }
}


int pomelo_protocol_packet_keep_alive_init(
    pomelo_protocol_packet_keep_alive_t * packet,
    pomelo_protocol_packet_keep_alive_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_KEEP_ALIVE;
    if (!info) return 0;

    packet->base.sequence = info->sequence;
    packet->client_id = info->client_id;

    return 0;
}


void pomelo_protocol_packet_keep_alive_cleanup(
    pomelo_protocol_packet_keep_alive_t * packet
) {
    (void) packet;
}


int pomelo_protocol_packet_disconnect_init(
    pomelo_protocol_packet_disconnect_t * packet,
    pomelo_protocol_packet_disconnect_info_t * info
) {
    assert(packet != NULL);
    packet->base.type = POMELO_PROTOCOL_PACKET_DISCONNECT;
    if (!info) return 0;

    packet->base.sequence = info->sequence;
    return 0;
}


void pomelo_protocol_packet_disconnect_cleanup(
    pomelo_protocol_packet_disconnect_t * packet
) {
    (void) packet;
}


bool pomelo_protocol_packet_validate_body_length(
    pomelo_protocol_packet_type type,
    size_t body_length,
    bool encrypted
) {
    size_t length = body_length;
    if (encrypted && type != POMELO_PROTOCOL_PACKET_REQUEST) {
        if (body_length < POMELO_HMAC_BYTES) {
            return false; // Too short to contain the auth tag
        }
        length -= POMELO_HMAC_BYTES;
    }

    switch (type) {
        case POMELO_PROTOCOL_PACKET_REQUEST:
            return (length == POMELO_PROTOCOL_PACKET_REQUEST_BODY_SIZE);

        case POMELO_PROTOCOL_PACKET_DENIED:
            return (length == POMELO_PROTOCOL_PACKET_DENIED_BODY_SIZE);

        case POMELO_PROTOCOL_PACKET_CHALLENGE:
            return (length == POMELO_PROTOCOL_PACKET_CHALLENGE_BODY_SIZE);

        case POMELO_PROTOCOL_PACKET_RESPONSE:
            return (length == POMELO_PROTOCOL_PACKET_RESPONSE_BODY_SIZE);

        case POMELO_PROTOCOL_PACKET_KEEP_ALIVE:
            return (length == POMELO_PROTOCOL_PACKET_KEEP_ALIVE_BODY_SIZE);

        case POMELO_PROTOCOL_PACKET_PAYLOAD:
            return (length > 0 && length <= POMELO_PACKET_BODY_CAPACITY);

        case POMELO_PROTOCOL_PACKET_DISCONNECT:
            return (length == POMELO_PROTOCOL_PACKET_DISCONNECT_BODY_SIZE);

        default:
            return false;
    }
}


int pomelo_protocol_packet_request_encode(
    pomelo_protocol_packet_request_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(view != NULL);
    (void) context;
    assert(packet != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = view->length;
    payload.capacity = view->buffer->capacity - view->offset;

    size_t remain = payload.capacity - payload.position;
    if (remain < POMELO_PROTOCOL_PACKET_REQUEST_BODY_SIZE) {
        return -1; // Not enough space
    }

    // version info (POMELO_VERSION_INFO_BYTES bytes)
    pomelo_payload_write_buffer_unsafe(
        &payload,
        (const uint8_t *) POMELO_VERSION_INFO,
        POMELO_VERSION_INFO_BYTES
    );

    // protocol id (8 bytes)
    pomelo_payload_write_uint64_unsafe(&payload, packet->protocol_id);

    // expire timestamp (8 bytes)
    pomelo_payload_write_uint64_unsafe(&payload, packet->expire_timestamp);

    // connect token nonce (POMELO_CONNECT_TOKEN_NONCE_BYTES bytes)
    pomelo_payload_write_buffer_unsafe(
        &payload,
        packet->connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    // encrypted priviate connect token data
    // (POMELO_CONNECT_TOKEN_PRIVATE_BYTES bytes)
    pomelo_payload_write_buffer_unsafe(
        &payload,
        packet->token_data.encrypted,
        POMELO_CONNECT_TOKEN_PRIVATE_BYTES
    );

    // Update the view length
    view->length = payload.position;
    return 0;
}


int pomelo_protocol_packet_request_decode(
    pomelo_protocol_packet_request_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    assert(context != NULL);
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = 0;
    payload.capacity = view->length;

    size_t remain = payload.capacity - payload.position;
    if (remain < POMELO_PROTOCOL_PACKET_REQUEST_BODY_SIZE) {
        return -1; // Not enough data
    }

    // Check version info
    int ret = memcmp(
        payload.data + payload.position,
        POMELO_VERSION_INFO,
        POMELO_VERSION_INFO_BYTES
    );
    if (ret != 0) {
        return -1; // Invalid version info
    }
    payload.position += POMELO_VERSION_INFO_BYTES;

    // protocol id
    pomelo_payload_read_uint64_unsafe(&payload, &packet->protocol_id);

    // expire timestamp
    pomelo_payload_read_uint64_unsafe(&payload, &packet->expire_timestamp);

    // token nonce
    pomelo_payload_read_buffer_unsafe(
        &payload,
        packet->connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    packet->token_data.token.protocol_id = packet->protocol_id;
    packet->token_data.token.expire_timestamp = packet->expire_timestamp;
    memcpy(
        packet->token_data.token.connect_token_nonce,
        packet->connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    // Decrypt encrypted private connect token data
    ret = pomelo_connect_token_decode_private(
        payload.data + payload.position,
        &packet->token_data.token,
        context->private_key
    );
    payload.position += POMELO_CONNECT_TOKEN_PRIVATE_BYTES;
    if (ret < 0) return ret;

    // Update the view offset and length
    view->offset += payload.position;
    view->length -= payload.position;

    return 0;
}


int pomelo_protocol_packet_challenge_encode(
    pomelo_protocol_packet_challenge_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    assert(context != NULL);
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = view->length;
    payload.capacity = view->buffer->capacity - view->offset;

    size_t remain = payload.capacity - payload.position;
    if (remain < POMELO_PROTOCOL_PACKET_CHALLENGE_BODY_SIZE) {
        return -1; // Not enough space
    }

    // Challenge token sequence
    pomelo_payload_write_uint64_unsafe(&payload, packet->token_sequence);

    // Challenge token data
    int ret = pomelo_codec_encrypt_challenge_token(
        &payload,
        &packet->challenge_data.token,
        context->challenge_key,
        packet->token_sequence
    );
    if (ret < 0) return ret;

    // Update the view length
    view->length = payload.position;
    return 0;
}


int pomelo_protocol_packet_challenge_decode(
    pomelo_protocol_packet_challenge_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    assert(context != NULL);
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = 0;
    payload.capacity = view->length;

    size_t remain = payload.capacity - payload.position;
    if (remain < POMELO_PROTOCOL_PACKET_CHALLENGE_BODY_SIZE) {
        return -1; // Not enough data
    }

    // Token sequence
    pomelo_payload_read_uint64_unsafe(&payload, &packet->token_sequence);

    // Encrypted challenge token
    pomelo_payload_read_buffer_unsafe(
        &payload,
        packet->challenge_data.encrypted,
        POMELO_CHALLENGE_TOKEN_BYTES
    );

    // Update the view offset and length
    view->offset += payload.position;
    view->length -= payload.position;

    return 0;
}


int pomelo_protocol_packet_response_encode(
    pomelo_protocol_packet_response_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    (void) context;
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = view->length;
    payload.capacity = view->buffer->capacity - view->offset;

    size_t remain = payload.capacity - payload.position;
    if (remain < POMELO_PROTOCOL_PACKET_RESPONSE_BODY_SIZE) {
        return -1; // Not enough space
    }

    // Token sequence
    pomelo_payload_write_uint64_unsafe(&payload, packet->token_sequence);

    // Encrypted challenge token
    pomelo_payload_write_buffer_unsafe(
        &payload,
        packet->challenge_data.encrypted,
        POMELO_CHALLENGE_TOKEN_BYTES
    );

    // Update the view length
    view->length = payload.position;
    return 0;
}


int pomelo_protocol_packet_response_decode(
    pomelo_protocol_packet_response_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    assert(context != NULL);
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = 0;
    payload.capacity = view->length;

    size_t remain = payload.capacity - payload.position;
    if (remain < POMELO_PROTOCOL_PACKET_RESPONSE_BODY_SIZE) {
        return -1; // Not enough data
    }

    // Token sequence
    pomelo_payload_read_uint64_unsafe(&payload, &packet->token_sequence);

    // Decrypt challenge token
    int ret = pomelo_codec_decrypt_challenge_token(
        &payload,
        &packet->challenge_data.token,
        context->challenge_key,
        packet->token_sequence
    );
    if (ret < 0) return ret;

    // Update the view offset and length
    view->offset += payload.position;
    view->length -= payload.position;
    return 0;
}


int pomelo_protocol_packet_keep_alive_encode(
    pomelo_protocol_packet_keep_alive_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    (void) context;
    assert(view != NULL);

    // Build payload
    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = view->length;
    payload.capacity = view->buffer->capacity - view->offset;

    // Write client ID
    int ret = pomelo_payload_write_int64(&payload, packet->client_id);
    if (ret < 0) return ret;

    // Update the view length
    view->length = payload.position;
    return 0;
}


int pomelo_protocol_packet_keep_alive_decode(
    pomelo_protocol_packet_keep_alive_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    (void) context;
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = 0;
    payload.capacity = view->length;

    // Read client ID
    int ret = pomelo_payload_read_int64(&payload, &packet->client_id);
    if (ret < 0) return ret;

    // Update the view offset and length
    view->offset += payload.position;
    view->length -= payload.position;
    return 0;
}


int pomelo_protocol_packet_payload_encode(
    pomelo_protocol_packet_payload_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    (void) context;
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = view->length;
    payload.capacity = view->buffer->capacity - view->offset;

    size_t nviews = packet->nviews;
    for (size_t i = 0; i < nviews; ++i) {
        pomelo_buffer_view_t * source = &packet->views[i];
        pomelo_buffer_t * buffer = source->buffer;
        assert(buffer != NULL);
        int ret = pomelo_payload_write_buffer(
            &payload,
            buffer->data + source->offset,
            source->length
        );
        if (ret < 0) return ret;
    }

    // Update the view length
    view->length = payload.position;
    return 0;
}


int pomelo_protocol_packet_payload_decode(
    pomelo_protocol_packet_payload_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    (void) context;
    assert(view != NULL);

    // Just attach the view to packet payload
    pomelo_protocol_packet_payload_attach_views(packet, view, 1);
    return 0;
}


void pomelo_protocol_packet_header_init(
    pomelo_protocol_packet_header_t * header,
    pomelo_protocol_packet_t * packet
) {
    assert(header != NULL);
    assert(packet != NULL);

    header->type = packet->type;
    if (packet->type == POMELO_PROTOCOL_PACKET_REQUEST) {
        // Only prefix zero for request packet
        header->prefix = 0;
        header->sequence = 0;
        header->sequence_bytes = 0;
        return;
    }

    size_t sequence_bytes =
        pomelo_payload_calc_packed_uint64_bytes(packet->sequence);
    header->sequence = packet->sequence;
    header->prefix = pomelo_protocol_prefix_encode(
        packet->type,
        sequence_bytes
    );
    header->sequence_bytes = sequence_bytes;
}


int pomelo_protocol_packet_header_encode(
    pomelo_protocol_packet_header_t * header,
    pomelo_buffer_view_t * view
) {
    assert(header != NULL);
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = view->length;
    payload.capacity = view->buffer->capacity - view->offset;

    if ((payload.capacity - payload.position) < (1 + header->sequence_bytes)) {
        return -1; // Not enough space
    }

    if (header->type == POMELO_PROTOCOL_PACKET_REQUEST) {
        // Only prefix zero for request packet
        pomelo_payload_write_uint8_unsafe(&payload, 0);
        assert(header->sequence_bytes == 0);
    } else {
        // prefix & sequence
        pomelo_payload_write_uint8_unsafe(&payload, header->prefix);
        pomelo_payload_write_packed_uint64_unsafe(
            &payload,
            header->sequence_bytes,
            header->sequence
        );
    }

    // Update the view length
    view->length += (1 + header->sequence_bytes);
    return 0;
}


int pomelo_protocol_packet_header_decode(
    pomelo_protocol_packet_header_t * header,
    pomelo_buffer_view_t * view
) {
    assert(header != NULL);
    assert(view != NULL);

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = 0;
    payload.capacity = view->length;

    // Read and decode prefix
    uint8_t prefix = 0;
    int ret = pomelo_payload_read_uint8(&payload, &prefix);
    if (ret < 0) return ret;
    header->prefix = prefix;

    if (prefix == 0) {
        header->type = POMELO_PROTOCOL_PACKET_REQUEST;
        header->sequence = 0;
        header->sequence_bytes = 0;
        view->offset += payload.position;
        view->length -= payload.position;
        return 0;
    }

    header->type = pomelo_protocol_prefix_decode_type(prefix);
    if (header->type >= POMELO_PROTOCOL_PACKET_TYPE_COUNT) {
        return -1; // Invalid packet type
    }

    uint8_t sequence_bytes =
        pomelo_protocol_prefix_decode_sequence_bytes(prefix);
    if (sequence_bytes > POMELO_PROTOCOL_SEQUENCE_BYTES_MAX ||
        sequence_bytes < POMELO_PROTOCOL_SEQUENCE_BYTES_MIN
    ) {
        return -1; // Invalid sequence bytes
    }
    header->sequence_bytes = sequence_bytes;

    ret = pomelo_payload_read_packed_uint64(
        &payload, sequence_bytes, &header->sequence
    );
    if (ret < 0) return ret;

    // Update the view offset and length
    size_t read_bytes = (1 + sequence_bytes);
    view->offset += read_bytes;
    view->length -= read_bytes;
    return 0;
}


int pomelo_protocol_packet_decode(
    pomelo_protocol_packet_t * packet,
    pomelo_protocol_crypto_context_t * crypto_ctx,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    assert(crypto_ctx != NULL);
    assert(view != NULL);

    switch (packet->type) {
        case POMELO_PROTOCOL_PACKET_REQUEST:
            return pomelo_protocol_packet_request_decode(
                (pomelo_protocol_packet_request_t *) packet, crypto_ctx, view
            );

        case POMELO_PROTOCOL_PACKET_CHALLENGE:
            return pomelo_protocol_packet_challenge_decode(
                (pomelo_protocol_packet_challenge_t *) packet, crypto_ctx, view
            );

        case POMELO_PROTOCOL_PACKET_RESPONSE:
            return pomelo_protocol_packet_response_decode(
                (pomelo_protocol_packet_response_t *) packet, crypto_ctx, view
            );

        case POMELO_PROTOCOL_PACKET_KEEP_ALIVE:
            return pomelo_protocol_packet_keep_alive_decode(
                (pomelo_protocol_packet_keep_alive_t *) packet, crypto_ctx, view
            );

        case POMELO_PROTOCOL_PACKET_PAYLOAD:
            return pomelo_protocol_packet_payload_decode(
                (pomelo_protocol_packet_payload_t *) packet, crypto_ctx, view
            );

        default:
            return 0;
    }
}


int pomelo_protocol_packet_encode(
    pomelo_protocol_packet_t * packet,
    pomelo_protocol_crypto_context_t * context,
    pomelo_buffer_view_t * view
) {
    assert(packet != NULL);
    assert(context != NULL);
    assert(view != NULL);

    switch (packet->type) {
        case POMELO_PROTOCOL_PACKET_REQUEST:
            return pomelo_protocol_packet_request_encode(
                (pomelo_protocol_packet_request_t *) packet, context, view
            );

        case POMELO_PROTOCOL_PACKET_CHALLENGE:
            return pomelo_protocol_packet_challenge_encode(
                (pomelo_protocol_packet_challenge_t *) packet, context, view
            );

        case POMELO_PROTOCOL_PACKET_RESPONSE:
            return pomelo_protocol_packet_response_encode(
                (pomelo_protocol_packet_response_t *) packet, context, view
            );

        case POMELO_PROTOCOL_PACKET_KEEP_ALIVE:
            return pomelo_protocol_packet_keep_alive_encode(
                (pomelo_protocol_packet_keep_alive_t *) packet, context, view
            );

        case POMELO_PROTOCOL_PACKET_PAYLOAD:
            return pomelo_protocol_packet_payload_encode(
                (pomelo_protocol_packet_payload_t *) packet, context, view
            );

        default:
            return 0;
    }
}
