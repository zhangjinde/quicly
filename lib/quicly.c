/*
 * Copyright (c) 2017 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "khash.h"
#include "quicly.h"

#define QUICLY_PROTOCOL_VERSION 1

#define QUICLY_PACKET_TYPE_VERSION_NEGOTIATION 1
#define QUICLY_PACKET_TYPE_CLIENT_INITIAL 2
#define QUICLY_PACKET_TYPE_SERVER_STATELESS_RETRY 3
#define QUICLY_PACKET_TYPE_SERVER_CLEARTEXT 4
#define QUICLY_PACKET_TYPE_CLIENT_CLEARTEXT 5
#define QUICLY_PACKET_TYPE_0RTT_PROTECTED 6
#define QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0 7
#define QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_1 8
#define QUICLY_PACKET_TYPE_PUBLIC_RESET 8
#define QUICLY_PACKET_TYPE_IS_VALID(type) ((uint8_t)(type)-1 < QUICLY_PACKET_TYPE_PUBLIC_RESET)

#define QUICLY_STREAM_HEADER_SIZE_MAX 14 /* (datalen16 + streamid32 + offset64) */

#define QUICLY_FRAME_TYPE_PADDING 0
#define QUICLY_FRAME_TYPE_STREAM 0xc0
#define QUICLY_FRAME_TYPE_STREAM_BIT_FIN 0x20
#define QUICLY_FRAME_TYPE_STREAM_BIT_DATA_LENGTH 0x10

#define QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS 26
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA 0
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA 1
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID 2
#define QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT 3
#define QUICLY_TRANSPORT_PARAMETER_ID_TRUNCATE_CONNECTION_ID 4

#define GET_TYPE_FROM_PACKET_HEADER(p) (*(uint8_t *)(p)&0x1f)

KHASH_MAP_INIT_INT(quicly_stream_t, quicly_stream_t *)

struct st_quicly_packet_protection_t {
    uint64_t packet_number;
    struct {
        ptls_aead_context_t *early_data;
        ptls_aead_context_t *key_phase0;
        ptls_aead_context_t *key_phase1;
    } aead;
    uint8_t secret[PTLS_MAX_DIGEST_SIZE];
};

struct st_quicly_conn_t {
    struct _st_quicly_conn_public_t super;
    khash_t(quicly_stream_t) * streams;
    struct {
        struct st_quicly_packet_protection_t pp;
    } ingress;
    struct {
        struct st_quicly_packet_protection_t pp;
    } egress;
    uint64_t in_packet_number;
    uint64_t out_packet_number;
};

struct st_quicly_crypto_stream_data_t {
    quicly_conn_t *conn;
    ptls_t *tls;
    ptls_handshake_properties_t handshake_properties;
    struct {
        ptls_raw_extension_t ext[2];
        ptls_buffer_t buf;
    } transport_parameters;
};

struct st_quicly_decoded_frame_t {
    uint8_t type;
    union {
        struct {
            uint32_t stream_id;
            unsigned fin : 1;
            uint64_t offset;
            ptls_iovec_t data;
        } stream;
    } data;
};

static uint16_t decode16(const uint8_t **src)
{
    uint16_t v = (uint16_t)(*src)[0] << 8 | (*src)[1];
    *src += 2;
    return v;
}

static uint32_t decode32(const uint8_t **src)
{
    uint32_t v = (uint32_t)(*src)[0] << 24 | (uint32_t)(*src)[1] << 16 | (uint32_t)(*src)[2] << 8 | (*src)[3];
    *src += 4;
    return v;
}

static uint64_t decode64(const uint8_t **src)
{
    uint64_t v = (uint64_t)(*src)[0] << 56 | (uint64_t)(*src)[1] << 48 | (uint64_t)(*src)[2] << 40 | (uint64_t)(*src)[3] << 32 |
                 (uint64_t)(*src)[4] << 24 | (uint64_t)(*src)[5] << 16 | (uint64_t)(*src)[6] << 8 | (*src)[7];
    *src += 8;
    return v;
}

static uint8_t *encode16(uint8_t *p, uint16_t v)
{
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static uint8_t *encode24(uint8_t *p, uint32_t v)
{
    *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static uint8_t *encode32(uint8_t *p, uint32_t v)
{
    *p++ = (uint8_t)(v >> 24);
    *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static uint8_t *encode64(uint8_t *p, uint64_t v)
{
    *p++ = (uint8_t)(v >> 56);
    *p++ = (uint8_t)(v >> 48);
    *p++ = (uint8_t)(v >> 40);
    *p++ = (uint8_t)(v >> 32);
    *p++ = (uint8_t)(v >> 24);
    *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static unsigned clz32(uint32_t v)
{
    QUICLY_BUILD_ASSERT(sizeof(unsigned) == 4);
    return v != 0 ? __builtin_clz(v) : 32;
}

static unsigned clz64(uint64_t v)
{
    QUICLY_BUILD_ASSERT(sizeof(long long) == 8);
    return v != 0 ? __builtin_clzll(v) : 64;
}

#define FNV1A_OFFSET_BASIS ((uint64_t)14695981039346656037u)

static uint64_t fnv1a(uint64_t hash, const uint8_t *p, const uint8_t *end)
{
    while (p != end) {
        hash = hash ^ (uint64_t)*p++;
        hash *= 1099511628211u;
    }

    return hash;
}

static int verify_cleartext_packet(quicly_decoded_packet_t *packet)
{
    uint64_t calced, received;
    const uint8_t *p;

    if (packet->payload.len < 8)
        return 0;
    packet->payload.len -= 8;

    calced = fnv1a(FNV1A_OFFSET_BASIS, packet->header.base, packet->header.base + packet->header.len);
    calced = fnv1a(calced, packet->payload.base, packet->payload.base + packet->payload.len);

    p = packet->payload.base + packet->payload.len;
    received = decode64(&p);

    return calced == received;
}

static void free_packet_protection(struct st_quicly_packet_protection_t *pp)
{
    if (pp->aead.early_data != NULL)
        ptls_aead_free(pp->aead.early_data);
    if (pp->aead.key_phase0 != NULL)
        ptls_aead_free(pp->aead.key_phase0);
    if (pp->aead.key_phase1 != NULL)
        ptls_aead_free(pp->aead.key_phase1);
}

int quicly_decode_packet(quicly_decoded_packet_t *packet, const uint8_t *src, size_t len)
{
    if (len < 2)
        return QUICLY_ERROR_INVALID_PACKET_HEADER;

    packet->header.base = (void *)src;

    const uint8_t *src_end = src + len;
    uint8_t first_byte = *src++;

    if ((first_byte & 0x80) != 0) {
        /* long header */
        packet->type = first_byte & 0x7f;
        packet->is_long_header = 1;
        packet->has_connection_id = 1;
        if (!QUICLY_PACKET_TYPE_IS_VALID(packet->type))
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        if (src_end - src < 16)
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        packet->connection_id = decode64(&src);
        packet->packet_number = decode32(&src);
        packet->version = decode32(&src);
    } else {
        /* short header */
        packet->type = (first_byte & 0x20) != 0 ? QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_1 : QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0;
        packet->is_long_header = 0;
        if ((first_byte & 0x40) != 0) {
            packet->has_connection_id = 1;
            if (src_end - src < 8)
                return QUICLY_ERROR_INVALID_PACKET_HEADER;
            packet->connection_id = decode64(&src);
        } else {
            packet->has_connection_id = 0;
        }
        switch (first_byte & 0x1f) {
        case 1:
            if (src_end - src < 1)
                return QUICLY_ERROR_INVALID_PACKET_HEADER;
            packet->packet_number = *src++;
            break;
        case 2:
            packet->packet_number = decode16(&src);
            break;
        case 3:
            packet->packet_number = decode32(&src);
            break;
        default:
            return QUICLY_ERROR_INVALID_PACKET_HEADER;
        }
    }

    packet->header.len = src - packet->header.base;
    packet->payload = ptls_iovec_init(src, src_end - src);
    return 0;
}

static int decode_frame(struct st_quicly_decoded_frame_t *frame, const uint8_t **src, const uint8_t *end)
{
    if (*src == end)
        goto CorruptFrame;

    uint8_t type_flags = *(*src)++;

    if (type_flags >= QUICLY_FRAME_TYPE_STREAM) {

        /* stream frame */
        frame->type = QUICLY_FRAME_TYPE_STREAM;

        /* record location of data_length */
        const uint8_t *data_length_at = NULL;
        if ((type_flags & QUICLY_FRAME_TYPE_STREAM_BIT_DATA_LENGTH) != 0) {
            if (end - *src < 2)
                goto CorruptFrame;
            data_length_at = *src;
            *src += 2;
        }

        { /* obtain stream_id */
            unsigned stream_id_length = ((type_flags >> 2) & 3) + 1;
            if (end - *src < stream_id_length)
                goto CorruptFrame;
            frame->data.stream.stream_id = 0;
            do {
                frame->data.stream.stream_id = (frame->data.stream.stream_id << 8) | *(*src)++;
            } while (--stream_id_length != 0);
        }

        { /* obtain offset */
            unsigned offset_length = (type_flags & 3) != 0 ? 1 << (type_flags & 3) : 0;
            if (end - *src < offset_length)
                goto CorruptFrame;
            frame->data.stream.offset = 0;
            for (; offset_length != 0; --offset_length)
                frame->data.stream.offset = (frame->data.stream.offset << 8) | *(*src)++;
        }

        /* obtain data */
        if (data_length_at != NULL) {
            uint16_t data_length = decode16(&data_length_at);
            if (end - *src < data_length)
                goto CorruptFrame;
            frame->data.stream.data = ptls_iovec_init(*src, data_length);
            *src += data_length;
        } else {
            frame->data.stream.data = ptls_iovec_init(*src, end - *src);
            *src = end;
        }

        /* fin bit */
        frame->data.stream.fin = (type_flags & QUICLY_FRAME_TYPE_STREAM_BIT_FIN) != 0;
        if (!frame->data.stream.fin && frame->data.stream.data.len == 0)
            return QUICLY_ERROR_EMPTY_STREAM_FRAME_NO_FIN;

        return 0;

    } else if (type_flags == QUICLY_FRAME_TYPE_PADDING) {

        frame->type = QUICLY_FRAME_TYPE_PADDING;
        return 0;

    } else {
        assert(!"FIXME");
    }

/* fallthru */
CorruptFrame:
    return QUICLY_ERROR_INVALID_FRAME_DATA;
}

static uint8_t *emit_long_header(quicly_conn_t *conn, uint8_t *dst, uint8_t type, uint64_t connection_id,
                                 uint32_t rounded_packet_number)
{
    *dst++ = 0x80 | type;
    dst = encode64(dst, connection_id);
    dst = encode32(dst, rounded_packet_number);
    dst = encode32(dst, QUICLY_PROTOCOL_VERSION);
    return dst;
}

static size_t encode_stream_frame_id_offset(uint8_t *type, uint8_t *dst, uint32_t stream_id, uint64_t offset)
{
    uint8_t *p = dst;

    *type = QUICLY_FRAME_TYPE_STREAM;

    {
        static const unsigned bits_table[] = {32, 24, 16, 8, 8};
        unsigned bits = bits_table[clz32(stream_id) / 8];
        *type |= (bits - 1) / 8;
        do {
            *p++ = (uint8_t)(stream_id >> (bits -= 8));
        } while (bits != 0);
    }

    if (offset != 0) {
        static const uint8_t flag_table[] = {3, 3, 2, 1};
        uint8_t flag = flag_table[clz64(offset) / 16];
        *type |= flag << 2;
        unsigned bits = 8 << flag;
        do {
            *p++ = (uint8_t)(offset >> (bits -= 8));
        } while (bits != 0);
    }

    return p - dst;
}

static int set_peeraddr(quicly_conn_t *conn, struct sockaddr *addr, socklen_t addrlen)
{
    int ret;

    if (conn->super.peer.salen != addrlen) {
        struct sockaddr *newsa;
        if ((newsa = malloc(addrlen)) == NULL) {
            ret = PTLS_ERROR_NO_MEMORY;
            goto Exit;
        }
        free(conn->super.peer.sa);
        conn->super.peer.sa = newsa;
        conn->super.peer.salen = addrlen;
    }

    memcpy(conn->super.peer.sa, addr, addrlen);
    ret = 0;

Exit:
    return ret;
}

static quicly_stream_t *open_stream(quicly_conn_t *conn, uint32_t stream_id)
{
    quicly_stream_t *stream;

    if ((stream = malloc(sizeof(*stream))) == NULL)
        return NULL;
    memset(stream, 0, sizeof(*stream));
    stream->stream_id = stream_id;
    ptls_buffer_init(&stream->sendbuf.buf, "", 0);

    int r;
    khiter_t iter = kh_put(quicly_stream_t, conn->streams, stream_id, &r);
    assert(iter != kh_end(conn->streams));
    kh_val(conn->streams, iter) = stream;

    return stream;
}

void destroy_stream(quicly_conn_t *conn, quicly_stream_t *stream)
{
    khiter_t iter = kh_get(quicly_stream_t, conn->streams, stream->stream_id);
    assert(iter != kh_end(conn->streams));
    kh_del(quicly_stream_t, conn->streams, iter);

    stream->on_receive(conn, stream, NULL, 0, 1);
    if (stream->sendbuf.buf.base != NULL)
        ptls_buffer_dispose(&stream->sendbuf.buf);
    free(stream);
}

quicly_stream_t *quicly_get_stream(quicly_conn_t *conn, uint32_t stream_id)
{
    khiter_t iter = kh_get(quicly_stream_t, conn->streams, stream_id);
    if (iter != kh_end(conn->streams))
        return kh_val(conn->streams, iter);
    return NULL;
}

void quicly_free(quicly_conn_t *conn)
{
    quicly_stream_t *stream;

    free_packet_protection(&conn->ingress.pp);
    free_packet_protection(&conn->egress.pp);

    kh_foreach_value(conn->streams, stream, { destroy_stream(conn, stream); });
    kh_destroy(quicly_stream_t, conn->streams);

    free(conn->super.peer.sa);
    free(conn);
}

static int crypto_stream_post_handshake_receive(quicly_conn_t *conn, quicly_stream_t *stream, ptls_iovec_t *vec, size_t veccnt,
                                                int fin)
{
    assert(!"FIXME");
}

static int setup_1rtt_secret(struct st_quicly_packet_protection_t *pp, ptls_t *tls, const char *label, int is_enc)
{
    ptls_cipher_suite_t *cipher = ptls_get_cipher(tls);
    int ret;

    if ((ret = ptls_export_secret(tls, pp->secret, cipher->hash->digest_size, label, ptls_iovec_init(NULL, 0))) != 0)
        return ret;
    if ((pp->aead.key_phase0 = ptls_aead_new(cipher->aead, cipher->hash, is_enc, pp->secret)) == NULL)
        return PTLS_ERROR_NO_MEMORY;
    return 0;
}

static int setup_1rtt(quicly_conn_t *conn, ptls_t *tls)
{
    int ret;

    if ((ret = setup_1rtt_secret(&conn->ingress.pp, tls,
                                 quicly_is_client(conn) ? "QUIC server 1-RTT Secret" : "QUIC client 1-RTT Secret", 0)) != 0)
        goto Exit;
    if ((ret = setup_1rtt_secret(&conn->egress.pp, tls,
                                 quicly_is_client(conn) ? "QUIC client 1-RTT Secret" : "QUIC server 1-RTT Secret", 1)) != 0)
        goto Exit;

    conn->super.state = QUICLY_STATE_1RTT_ENCRYPTED;

Exit:
    return 0;
}

static int crypto_stream_on_handshake_receive(quicly_conn_t *conn, quicly_stream_t *stream, ptls_iovec_t *vec, size_t veccnt,
                                              int fin)
{
    struct st_quicly_crypto_stream_data_t *data = stream->data;
    size_t i;
    int ret;

    if (fin) {
        ret = QUICLY_ERROR_INVALID_FRAME_DATA;
        goto Exit;
    }

    ret = PTLS_ERROR_IN_PROGRESS;
    for (i = 0; i < veccnt; ++i) {
        size_t inlen = vec[0].len;
        if ((ret = ptls_handshake(data->tls, &stream->sendbuf.buf, vec[0].base, &inlen, &data->handshake_properties)) !=
            PTLS_ERROR_IN_PROGRESS)
            break;
    }

    switch (ret) {
    case 0:
        stream->on_receive = crypto_stream_post_handshake_receive;
        /* state is 1RTT_ENCRYPTED when handling ClientFinished */
        if (conn->super.state < QUICLY_STATE_1RTT_ENCRYPTED) {
            if ((ret = setup_1rtt(conn, data->tls)) != 0)
                goto Exit;
        }
        break;
    case PTLS_ERROR_IN_PROGRESS:
        if (conn->super.state == QUICLY_STATE_BEFORE_SH)
            conn->super.state = QUICLY_STATE_BEFORE_SF;
        ret = 0;
        break;
    default:
        break;
    }

Exit:
    return ret;
}

#define PUSH_TRANSPORT_PARAMETER(buf, id, block)                                                                                   \
    do {                                                                                                                           \
        ptls_buffer_push16((buf), (id));                                                                                           \
        ptls_buffer_push_block((buf), 2, block);                                                                                   \
    } while (0)

static int encode_transport_parameter_list(quicly_transport_parameters_t *params, ptls_buffer_t *buf)
{
    int ret;

    ptls_buffer_push_block(buf, 2, {
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA,
                                 { ptls_buffer_push32(buf, params->initial_max_stream_data); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA,
                                 { ptls_buffer_push32(buf, params->initial_max_data); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID,
                                 { ptls_buffer_push32(buf, params->initial_max_stream_id); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT,
                                 { ptls_buffer_push16(buf, params->idle_timeout); });
        if (params->truncate_connection_id)
            PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_TRUNCATE_CONNECTION_ID, {});
    });
    ret = 0;
Exit:
    return ret;
}

static int decode_transport_parameter_list(quicly_transport_parameters_t *params, const uint8_t *src, const uint8_t *end)
{
#define ID_TO_BIT(id) ((uint64_t)1 << (id))

    uint64_t found_id_bits = 0,
             must_found_id_bits = ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT);
    int ret;

    /* set optional parameters to their default values */
    params->truncate_connection_id = 0;

    /* decode the parameters block */
    ptls_decode_block(src, end, 2, {
        while (src != end) {
            uint16_t id;
            if ((ret = ptls_decode16(&id, &src, end)) != 0)
                goto Exit;
            if (id < sizeof(found_id_bits) * 8) {
                if ((found_id_bits & ID_TO_BIT(id)) != 0) {
                    ret = QUICLY_ERROR_INVALID_STREAM_DATA; /* FIXME error code */
                    goto Exit;
                }
                found_id_bits |= ID_TO_BIT(id);
            }
            found_id_bits |= ID_TO_BIT(id);
            ptls_decode_open_block(src, end, 2, {
                switch (id) {
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA:
                    if ((ret = ptls_decode32(&params->initial_max_stream_data, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA:
                    if ((ret = ptls_decode32(&params->initial_max_data, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_ID:
                    if ((ret = ptls_decode32(&params->initial_max_stream_id, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT:
                    if ((ret = ptls_decode16(&params->idle_timeout, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_TRUNCATE_CONNECTION_ID:
                    params->truncate_connection_id = 1;
                    break;
                default:
                    src = end;
                    break;
                }
            });
        }
    });

    /* check that we have found all the required parameters */
    if ((found_id_bits & must_found_id_bits) != must_found_id_bits) {
        ret = QUICLY_ERROR_INVALID_STREAM_DATA; /* FIXME error code */
        goto Exit;
    }

    ret = 0;
Exit:
    return ret;

#undef ID_TO_BIT
}

static int collect_transport_parameters(ptls_t *tls, struct st_ptls_handshake_properties_t *properties, uint16_t type)
{
    return type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS;
}

static quicly_conn_t *create_connection(quicly_context_t *ctx, const char *server_name, struct sockaddr *sa, socklen_t salen,
                                        ptls_handshake_properties_t *handshake_properties, quicly_stream_t **crypto_stream)
{
    quicly_conn_t *conn;
    struct st_quicly_crypto_stream_data_t *crypto_data;

    *crypto_stream = NULL;

    if ((conn = malloc(sizeof(*conn))) == NULL)
        return NULL;

    memset(conn, 0, sizeof(*conn));
    conn->super.ctx = ctx;
    conn->streams = kh_init(quicly_stream_t);
    if (set_peeraddr(conn, sa, salen) != 0)
        goto Fail;

    /* instantiate the crypto stream */
    if ((*crypto_stream = open_stream(conn, 0)) == NULL)
        goto Fail;
    if ((crypto_data = malloc(sizeof(*crypto_data))) == NULL)
        goto Fail;
    (*crypto_stream)->data = crypto_data;
    (*crypto_stream)->on_receive = crypto_stream_on_handshake_receive;
    crypto_data->conn = conn;
    if ((crypto_data->tls = ptls_new(ctx->tls, server_name == NULL)) == NULL)
        goto Fail;
    if (server_name != NULL && ptls_set_server_name(crypto_data->tls, server_name, strlen(server_name)) != 0)
        goto Fail;
    if (handshake_properties != NULL) {
        assert(handshake_properties->additional_extensions == NULL);
        assert(handshake_properties->collect_extension == NULL);
        assert(handshake_properties->collected_extensions == NULL);
        crypto_data->handshake_properties = *handshake_properties;
    } else {
        crypto_data->handshake_properties = (ptls_handshake_properties_t){{{NULL}}};
    }
    crypto_data->handshake_properties.collect_extension = collect_transport_parameters;
    if (server_name != NULL) {
        conn->super.host.next_stream_id = 1;
        conn->super.peer.next_stream_id = 2;
    } else {
        conn->super.host.next_stream_id = 2;
        conn->super.peer.next_stream_id = 1;
    }

    return conn;
Fail:
    if (conn != NULL)
        quicly_free(conn);
    return NULL;
}

static int client_collected_extensions(ptls_t *tls, ptls_handshake_properties_t *properties, ptls_raw_extension_t *slots)
{
    struct st_quicly_crypto_stream_data_t *crypto_data =
        (void *)((char *)properties - offsetof(struct st_quicly_crypto_stream_data_t, handshake_properties));
    int ret;

    if (slots[0].type == UINT16_MAX) {
        ret = PTLS_ALERT_MISSING_EXTENSION;
        goto Exit;
    }
    assert(slots[0].type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS);
    assert(slots[1].type == UINT16_MAX);

    const uint8_t *src = slots[0].data.base, *end = src + slots[0].data.len;

    ptls_decode_open_block(src, end, 1, {
        int found_negotiated_version = 0;
        do {
            uint32_t supported_version;
            if ((ret = ptls_decode32(&supported_version, &src, end)) != 0)
                goto Exit;
            if (supported_version == QUICLY_PROTOCOL_VERSION)
                found_negotiated_version = 1;
        } while (src != end);
        if (!found_negotiated_version) {
            ret = PTLS_ALERT_ILLEGAL_PARAMETER; /* FIXME is this the correct error code? */
            goto Exit;
        }
    });
    ret = decode_transport_parameter_list(&crypto_data->conn->super.peer.transport_params, src, end);

Exit:
    return ret;
}

int quicly_connect(quicly_conn_t **_conn, quicly_context_t *ctx, const char *server_name, struct sockaddr *sa, socklen_t salen,
                   ptls_handshake_properties_t *handshake_properties)
{
    quicly_conn_t *conn;
    quicly_stream_t *crypto_stream;
    struct st_quicly_crypto_stream_data_t *crypto_data;
    int ret;

    if ((conn = create_connection(ctx, server_name, sa, salen, handshake_properties, &crypto_stream)) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }
    crypto_data = crypto_stream->data;

    /* handshake */
    ptls_buffer_init(&crypto_data->transport_parameters.buf, "", 0);
    ptls_buffer_push32(&crypto_data->transport_parameters.buf, QUICLY_PROTOCOL_VERSION);
    ptls_buffer_push32(&crypto_data->transport_parameters.buf, QUICLY_PROTOCOL_VERSION);
    if ((ret = encode_transport_parameter_list(&ctx->transport_params, &crypto_data->transport_parameters.buf)) != 0)
        goto Exit;
    crypto_data->transport_parameters.ext[0] =
        (ptls_raw_extension_t){QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS,
                               {crypto_data->transport_parameters.buf.base, crypto_data->transport_parameters.buf.off}};
    crypto_data->transport_parameters.ext[1] = (ptls_raw_extension_t){UINT16_MAX};
    crypto_data->handshake_properties.additional_extensions = crypto_data->transport_parameters.ext;
    crypto_data->handshake_properties.collected_extensions = client_collected_extensions;
    if ((ret = ptls_handshake(crypto_data->tls, &crypto_stream->sendbuf.buf, NULL, 0, &crypto_data->handshake_properties)) !=
        PTLS_ERROR_IN_PROGRESS)
        goto Exit;

    *_conn = conn;
    ret = 0;

Exit:
    if (ret != 0) {
        if (conn != NULL)
            quicly_free(conn);
    }
    return ret;
}

static int server_collected_extensions(ptls_t *tls, ptls_handshake_properties_t *properties, ptls_raw_extension_t *slots)
{
    struct st_quicly_crypto_stream_data_t *crypto_data =
        (void *)((char *)properties - offsetof(struct st_quicly_crypto_stream_data_t, handshake_properties));
    int ret;

    if (slots[0].type == UINT16_MAX) {
        ret = PTLS_ALERT_MISSING_EXTENSION;
        goto Exit;
    }
    assert(slots[0].type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS);
    assert(slots[1].type == UINT16_MAX);

    { /* decode transport_parameters extension */
        const uint8_t *src = slots[0].data.base, *end = src + slots[0].data.len;
        uint32_t negotiated_version, initial_version;
        if ((ret = ptls_decode32(&negotiated_version, &src, end)) != 0)
            goto Exit;
        if ((ret = ptls_decode32(&initial_version, &src, end)) != 0)
            goto Exit;
        if (!(negotiated_version == QUICLY_PROTOCOL_VERSION && initial_version == QUICLY_PROTOCOL_VERSION)) {
            ret = QUICLY_ERROR_VERSION_NEGOTIATION_MISMATCH;
            goto Exit;
        }
        if ((ret = decode_transport_parameter_list(&crypto_data->conn->super.peer.transport_params, src, end)) != 0)
            goto Exit;
    }

    /* set transport_parameters extension to be sent in EE */
    assert(properties->additional_extensions == NULL);
    ptls_buffer_init(&crypto_data->transport_parameters.buf, "", 0);
    ptls_buffer_push_block(&crypto_data->transport_parameters.buf, 1,
                           { ptls_buffer_push32(&crypto_data->transport_parameters.buf, QUICLY_PROTOCOL_VERSION); });
    if ((ret = encode_transport_parameter_list(&crypto_data->conn->super.ctx->transport_params,
                                               &crypto_data->transport_parameters.buf)) != 0)
        goto Exit;
    properties->additional_extensions = crypto_data->transport_parameters.ext;
    crypto_data->transport_parameters.ext[0] =
        (ptls_raw_extension_t){QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS,
                               {crypto_data->transport_parameters.buf.base, crypto_data->transport_parameters.buf.off}};
    crypto_data->transport_parameters.ext[1] = (ptls_raw_extension_t){UINT16_MAX};
    crypto_data->handshake_properties.additional_extensions = crypto_data->transport_parameters.ext;

    ret = 0;

Exit:
    return ret;
}

int quicly_accept(quicly_conn_t **_conn, quicly_context_t *ctx, struct sockaddr *sa, socklen_t salen,
                  ptls_handshake_properties_t *handshake_properties, quicly_decoded_packet_t *packet)
{
    quicly_conn_t *conn = NULL;
    quicly_stream_t *crypto_stream;
    struct st_quicly_crypto_stream_data_t *crypto_data;
    struct st_quicly_decoded_frame_t frame;
    int ret;

    /* ignore any packet that does not  */
    if (packet->type != QUICLY_PACKET_TYPE_CLIENT_INITIAL) {
        ret = QUICLY_ERROR_PACKET_IGNORED;
        goto Exit;
    }
    if (!verify_cleartext_packet(packet)) {
        ret = QUICLY_ERROR_DECRYPTION_FAILURE;
        goto Exit;
    }
    {
        const uint8_t *p = packet->payload.base, *end = p + packet->payload.len;
        if ((ret = decode_frame(&frame, &p, end)) != 0)
            goto Exit;
        if (frame.type != QUICLY_FRAME_TYPE_STREAM) {
            ret = QUICLY_ERROR_INVALID_FRAME_DATA;
            goto Exit;
        }
        if (frame.data.stream.offset != 0) {
            ret = QUICLY_ERROR_INVALID_STREAM_DATA;
            goto Exit;
        }
        /* FIXME check packet size */
        for (; p < end; ++p) {
            if (*p != QUICLY_FRAME_TYPE_PADDING) {
                ret = QUICLY_ERROR_INVALID_FRAME_DATA;
                goto Exit;
            }
        }
    }

    if ((conn = create_connection(ctx, NULL, sa, salen, handshake_properties, &crypto_stream)) == NULL)
        return PTLS_ERROR_NO_MEMORY;
    crypto_data = crypto_stream->data;

    { /* handshake */
        size_t len = frame.data.stream.data.len;
        crypto_data->handshake_properties.collected_extensions = server_collected_extensions;
        ret = ptls_handshake(crypto_data->tls, &crypto_stream->sendbuf.buf, frame.data.stream.data.base, &len,
                             &crypto_data->handshake_properties);
        switch (ret) {
        case 0:
        case PTLS_ERROR_IN_PROGRESS:
            if (len != frame.data.stream.data.len) {
                ret = QUICLY_ERROR_INVALID_STREAM_DATA;
                goto Exit;
            }
            if (ret == 0) {
                if ((ret = setup_1rtt(conn, crypto_data->tls)) != 0)
                    goto Exit;
            } else {
                conn->super.state = QUICLY_STATE_BEFORE_SF;
            }
            break;
        default:
            goto Exit;
        }
    }

    *_conn = conn;

Exit:
    if (!(ret == 0 || ret == PTLS_ERROR_IN_PROGRESS)) {
        if (conn != NULL)
            quicly_free(conn);
    }
    return ret;
}

struct st_quicly_send_context_t {
    uint8_t packet_type;
    ptls_aead_context_t *aead;
    quicly_raw_packet_t **packets;
    size_t max_packets;
    size_t num_packets;
    quicly_raw_packet_t *target;
    uint8_t *dst;
    uint8_t *dst_end;
};

static void commit_send_packet(struct st_quicly_send_context_t *s)
{
    if (s->aead != NULL) {
        s->dst += ptls_aead_encrypt_final(s->aead, s->dst);
    } else {
        uint64_t hash = fnv1a(FNV1A_OFFSET_BASIS, s->target->data.base, s->dst);
        s->dst = encode64(s->dst, hash);
    }
    s->target->data.len = s->dst - s->target->data.base;
    s->packets[s->num_packets++] = s->target;
    s->target = NULL;
    s->dst = NULL;
    s->dst_end = NULL;
}

static int send_core(quicly_conn_t *conn, quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    int ret = 0, send_fin = stream->send_fin;

    while (stream->sendbuf.unacked != stream->sendbuf.buf.off || send_fin) {
        uint8_t type;
        uint8_t encoded_id_offset[QUICLY_STREAM_HEADER_SIZE_MAX];
        size_t encoded_id_offset_size = encode_stream_frame_id_offset(&type, encoded_id_offset, stream->stream_id, stream->offset);
        size_t min_required_space = encoded_id_offset_size + (stream->sendbuf.buf.off != 0);

        /* allocate and setup the new packet if necessary */
        if (s->dst_end - s->dst < min_required_space || GET_TYPE_FROM_PACKET_HEADER(s->target->data.base) != s->packet_type) {
            if (s->target != NULL) {
                while (s->dst != s->dst_end)
                    *s->dst++ = QUICLY_FRAME_TYPE_PADDING;
                commit_send_packet(s);
            }
            if (s->num_packets >= s->max_packets)
                break;
            if ((s->target = conn->super.ctx->alloc_packet(conn->super.ctx, conn->super.peer.salen,
                                                           conn->super.ctx->max_packet_size)) == NULL) {
                ret = PTLS_ERROR_NO_MEMORY;
                break;
            }
            s->target->salen = conn->super.peer.salen;
            memcpy(&s->target->sa, &conn->super.peer.sa, conn->super.peer.salen);
            s->dst = s->target->data.base;
            s->dst_end = s->target->data.base + conn->super.ctx->max_packet_size;
            s->dst = emit_long_header(conn, s->dst, s->packet_type, conn->super.connection_id, (uint32_t)conn->out_packet_number);
            if (s->aead != NULL) {
                s->dst_end -= s->aead->algo->tag_size;
                ptls_aead_encrypt_init(s->aead, conn->out_packet_number, s->target->data.base, s->dst - s->target->data.base);
            } else {
                s->dst_end -= 8; /* space for fnv1a-64 */
            }
            assert(s->dst < s->dst_end);
            ++conn->out_packet_number;
        }

        uint8_t *type_at = s->dst++;

        /* emit stream header as well as calculating the size of the data to send */
        size_t capacity = s->dst_end - s->dst - encoded_id_offset_size;
        size_t avail = stream->sendbuf.buf.off - stream->sendbuf.unacked;
        size_t copysize;
        if (capacity <= avail) {
            copysize = capacity;
        } else {
            copysize = avail;
            /* set data_length if there's a chance to store more frames in the current packet */
            if (capacity - avail > 2) {
                type |= QUICLY_FRAME_TYPE_STREAM_BIT_DATA_LENGTH;
                s->dst = encode16(s->dst, (uint16_t)copysize);
            }
        }
        if (stream->sendbuf.unacked + copysize == stream->sendbuf.buf.off && stream->send_fin)
            type |= QUICLY_FRAME_TYPE_STREAM_BIT_FIN;
        *type_at = type;
        memcpy(s->dst, encoded_id_offset, encoded_id_offset_size);
        s->dst += encoded_id_offset_size;

        /* encrypt stream header if necessary */
        if (s->aead != NULL)
            ptls_aead_encrypt_update(s->aead, type_at, type_at, s->dst - type_at);

        /* emit data */
        if (copysize != 0) {
            if (s->aead != NULL) {
                ptls_aead_encrypt_update(s->aead, s->dst, stream->sendbuf.buf.base + stream->sendbuf.unacked, copysize);
            } else {
                memcpy(s->dst, stream->sendbuf.buf.base + stream->sendbuf.unacked, copysize);
            }
            s->dst += copysize;
            stream->sendbuf.unacked += copysize;
        }

        /* need to send only once (per loop) */
        send_fin = 0;
    }

    return ret;
}

static int send_crytpo_stream(quicly_conn_t *conn, quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    int ret;

    if (quicly_is_client(conn)) {
        if (conn->super.state == QUICLY_STATE_BEFORE_SH) {
            s->packet_type = QUICLY_PACKET_TYPE_CLIENT_INITIAL;
        } else {
            s->packet_type = QUICLY_PACKET_TYPE_CLIENT_CLEARTEXT;
        }
    } else {
        s->packet_type = QUICLY_PACKET_TYPE_SERVER_CLEARTEXT;
    }
    s->aead = NULL;

    if ((ret = send_core(conn, stream, s)) != 0)
        return ret;

    if (s->target != NULL && s->packet_type == QUICLY_PACKET_TYPE_CLIENT_INITIAL) {
        if (s->num_packets != 0)
            return QUICLY_ERROR_HANDSHAKE_TOO_LARGE;
        const size_t max_size = 1272; /* max UDP packet size excluding fnv1a hash */
        assert(s->dst - s->target->data.base <= max_size);
        memset(s->dst, 0, s->target->data.base + max_size - s->dst);
        s->dst = s->target->data.base + max_size;
        commit_send_packet(s);
    }

    return 0;
}

static int send_non_crypto_stream(quicly_conn_t *conn, quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    if (conn->super.state < QUICLY_STATE_1RTT_ENCRYPTED) {
        assert(quicly_is_client(conn));
        s->packet_type = QUICLY_PACKET_TYPE_0RTT_PROTECTED;
        s->aead = conn->egress.pp.aead.early_data;
    } else {
        s->packet_type = QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0;
        s->aead = conn->egress.pp.aead.key_phase0;
    }

    return send_core(conn, stream, s);
}

int quicly_send(quicly_conn_t *conn, quicly_raw_packet_t **packets, size_t *num_packets)
{
    quicly_stream_t *stream;
    struct st_quicly_send_context_t s = {UINT8_MAX, NULL, packets, *num_packets};
    int ret;

    /* send crypto stream */
    if ((ret = send_crytpo_stream(conn, quicly_get_stream(conn, 0), &s)) != 0)
        goto Exit;

    /* TODO send acks */

    if (s.target != NULL)
        commit_send_packet(&s);

    /* send data in other streams (TODO respect priorities between streams) */
    kh_foreach_value(conn->streams, stream, {
        if (stream->stream_id != 0) {
            if ((ret = send_non_crypto_stream(conn, stream, &s)) != 0)
                goto Exit;
        }
    });
    if (s.target != NULL)
        commit_send_packet(&s);

    *num_packets = s.num_packets;
    ret = 0;
Exit:
    return ret;
}

static int handle_stream_frame(quicly_conn_t *conn, struct st_quicly_decoded_frame_t *frame)
{
    quicly_stream_t *stream = quicly_get_stream(conn, frame->data.stream.stream_id);
    int ret = 0;

    /* open new stream(s) if necessary */
    if (stream == NULL && frame->data.stream.stream_id % 2 != quicly_is_client(conn) &&
        (conn->super.peer.next_stream_id != 0 && conn->super.peer.next_stream_id <= frame->data.stream.stream_id)) {
        do {
            if ((stream = open_stream(conn, conn->super.peer.next_stream_id)) == NULL) {
                ret = PTLS_ERROR_NO_MEMORY;
                goto Exit;
            }
            if ((ret = conn->super.ctx->on_stream_open(conn->super.ctx, conn, stream)) != 0)
                goto Exit;
            conn->super.peer.next_stream_id += 2;
        } while (frame->data.stream.stream_id != stream->stream_id);
        /* disallow opening new streams if the number has overlapped */
        if (conn->super.peer.next_stream_id < 2)
            conn->super.peer.next_stream_id = 0;
    }

    if (stream != NULL) {
        ptls_iovec_t v = ptls_iovec_init(frame->data.stream.data.base, frame->data.stream.data.len);
        if ((ret = stream->on_receive(conn, stream, &v, v.len != 0, frame->data.stream.fin)) != 0)
            goto Exit;
    }

Exit:
    return ret;
}

int quicly_receive(quicly_conn_t *conn, quicly_decoded_packet_t *packet)
{
    ptls_aead_context_t *aead = NULL;
    int ret;

    /* ignore packets having wrong connection id */
    if (packet->connection_id != conn->super.connection_id) {
        ret = QUICLY_ERROR_PACKET_IGNORED;
        goto Exit;
    }

    if (!packet->is_long_header && conn->super.state != QUICLY_STATE_1RTT_ENCRYPTED) {
        ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
        goto Exit;
    }

    switch (packet->type) {
    case QUICLY_PACKET_TYPE_CLIENT_CLEARTEXT:
        if (quicly_is_client(conn)) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_SERVER_CLEARTEXT:
        if (!quicly_is_client(conn)) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_0RTT_PROTECTED:
        if (quicly_is_client(conn) || (aead = conn->ingress.pp.aead.early_data) == NULL) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_0:
        if ((aead = conn->ingress.pp.aead.key_phase0) == NULL) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    case QUICLY_PACKET_TYPE_1RTT_KEY_PHASE_1:
        if ((aead = conn->ingress.pp.aead.key_phase1) == NULL) {
            ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
            goto Exit;
        }
        break;
    default:
        ret = QUICLY_ERROR_INVALID_PACKET_HEADER;
        goto Exit;
    }

    if (aead != NULL) {
        if ((packet->payload.len = ptls_aead_decrypt(aead, packet->payload.base, packet->payload.base, packet->payload.len,
                                                     packet->packet_number, packet->header.base, packet->header.len)) == SIZE_MAX) {
            ret = QUICLY_ERROR_DECRYPTION_FAILURE;
            goto Exit;
        }
    } else {
        if (!verify_cleartext_packet(packet)) {
            ret = QUICLY_ERROR_DECRYPTION_FAILURE;
            goto Exit;
        }
    }

    const uint8_t *p = packet->payload.base, *end = p + packet->payload.len;
    do {
        struct st_quicly_decoded_frame_t frame;
        if ((ret = decode_frame(&frame, &p, end)) != 0)
            goto Exit;
        switch (frame.type) {
        case QUICLY_FRAME_TYPE_PADDING:
            break;
        case QUICLY_FRAME_TYPE_STREAM:
            if ((ret = handle_stream_frame(conn, &frame)) != 0)
                goto Exit;
            break;
        default:
            assert(!"FIXME");
            break;
        }
    } while (p != end);

Exit:
    return ret;
}

int quicly_open_stream(quicly_conn_t *conn, quicly_stream_t **stream)
{
    if (conn->super.host.next_stream_id == 0)
        return QUICLY_ERROR_TOO_MANY_OPEN_STREAMS;

    if ((*stream = open_stream(conn, conn->super.host.next_stream_id)) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    if ((conn->super.host.next_stream_id += 2) < 2)
        conn->super.host.next_stream_id = 0;

    return 0;
}

quicly_raw_packet_t *quicly_default_alloc_packet(quicly_context_t *ctx, socklen_t salen, size_t payloadsize)
{
    quicly_raw_packet_t *packet;

    if ((packet = malloc(offsetof(quicly_raw_packet_t, sa) + salen + payloadsize)) == NULL)
        return NULL;
    packet->salen = salen;
    packet->data.base = (uint8_t *)packet + offsetof(quicly_raw_packet_t, sa) + salen;

    return packet;
}

void quicly_default_free_packet(quicly_context_t *ctx, quicly_raw_packet_t *packet)
{
    free(packet);
}