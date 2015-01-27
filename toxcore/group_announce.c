/*
 * group_announce.h -- Similar to ping.h, but designed for group chat purposes
 *
 *  Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#include "group_announce.h"

#include "logger.h"
#include "util.h"
#include "network.h"
#include "DHT.h"

#define MAX_GCA_PACKET_SIZE 65507
#define TIME_STAMP_SIZE (sizeof(uint64_t))
#define RAND_ID_SIZE (sizeof(uint64_t))

/* type + sender_dht_pk + nonce + */
#define GCA_HEADER_SIZE (1 + ENC_PUBLIC_KEY + crypto_box_NONCEBYTES)

/* type + ping_id */
#define GCA_PING_REQUEST_PLAIN_SIZE (1 + RAND_ID_SIZE)
#define GCA_PING_REQUEST_DHT_SIZE (GCA_HEADER_SIZE + ENC_PUBLIC_KEY + GCA_PING_REQUEST_PLAIN_SIZE + crypto_box_MACBYTES)

/* type + ping_id */
#define GCA_PING_RESPONSE_PLAIN_SIZE (1 + RAND_ID_SIZE)
#define GCA_PING_RESPONSE_DHT_SIZE (GCA_HEADER_SIZE + GCA_PING_RESPONSE_PLAIN_SIZE + crypto_box_MACBYTES)

#define GCA_PING_INTERVAL 60
#define GCA_NODES_EXPIRATION (GCA_PING_INTERVAL * 3 + 10)

#define MAX_GCA_SENT_NODES 4
#define MAX_GCA_ANNOUNCED_NODES 30

/* Holds nodes that we receive when we send a request, used to join groups */
struct GC_AnnounceRequest {
    uint8_t chat_id[EXT_PUBLIC_KEY];
    GC_Announce_Node nodes[MAX_GCA_SENT_NODES];
    uint64_t req_id;
    uint64_t time_added;
    bool ready;

    uint8_t long_pk[EXT_PUBLIC_KEY];
    uint8_t long_sk[EXT_SECRET_KEY];
};

/* Holds announced nodes we get via announcements */
struct GC_AnnouncedNode {
    uint8_t chat_id[EXT_PUBLIC_KEY];
    GC_Announce_Node node;
    uint64_t last_rcvd_ping;
    uint64_t last_sent_ping;
    uint64_t time_added;
    uint64_t ping_id;
};

typedef struct GC_Announce {
    DHT *dht;

    struct GC_AnnouncedNode announcements[MAX_GCA_ANNOUNCED_NODES];
    struct GC_AnnounceRequest self_requests[MAX_GCA_SELF_REQUESTS];
} GC_Announce;


/* Copies your own ip_port structure to target. (TODO: This should probably go somewhere else)
 *
 * Return 0 on succcess.
 * Return -1 on failure.
 */
static int ipport_self_copy(const DHT *dht, IP_Port *target)
{
    int i;

    for (i = 0; i < LCLIENT_LIST; i++) {
        if (ipport_isset(&dht->close_clientlist[i].assoc4.ret_ip_port)) {
            ipport_copy(target, &dht->close_clientlist[i].assoc4.ret_ip_port);
            break;
        }

        if (ipport_isset(&dht->close_clientlist[i].assoc6.ret_ip_port)) {
            ipport_copy(target, &dht->close_clientlist[i].assoc6.ret_ip_port);
            break;
        }
    }

    if (!ipport_isset(target))
        return -1;

    return 0;
}

/* Creates a GC_Announce_Node using client_id and your own IP_Port struct
 *
 * Return 0 on success.
 * Return -1 on failure.
 */
int make_self_gca_node(const DHT *dht, GC_Announce_Node *node, const uint8_t *client_id)
{
    IP_Port self_node;
    if (ipport_self_copy(dht, &node->ip_port) == -1)
        return -1;

    memcpy(node->client_id, client_id, EXT_PUBLIC_KEY);
    return 0;
}

/* Pack number of nodes into data of maxlength length.
 *
 * return length of packed nodes on success.
 * return -1 on failure.
 */
int pack_gca_nodes(uint8_t *data, uint16_t length, const GC_Announce_Node *nodes, uint16_t number)
{
    uint32_t i, packed_length = 0;

    for (i = 0; i < number; ++i) {
        int ipv6 = -1;
        uint8_t net_family;

        if (nodes[i].ip_port.ip.family == AF_INET) {
            ipv6 = 0;
            net_family = TOX_AF_INET;
        } else if (nodes[i].ip_port.ip.family == TCP_INET) {
            ipv6 = 0;
            net_family = TOX_TCP_INET;
        } else if (nodes[i].ip_port.ip.family == AF_INET6) {
            ipv6 = 1;
            net_family = TOX_AF_INET6;
        } else if (nodes[i].ip_port.ip.family == TCP_INET6) {
            ipv6 = 1;
            net_family = TOX_TCP_INET6;
        } else {
            return -1;
        }

        if (ipv6 == 0) {
            uint32_t size = 1 + sizeof(IP4) + sizeof(uint16_t) + EXT_PUBLIC_KEY;

            if (packed_length + size > length)
                return -1;

            data[packed_length] = net_family;
            memcpy(data + packed_length + 1, &nodes[i].ip_port.ip.ip4, sizeof(IP4));
            memcpy(data + packed_length + 1 + sizeof(IP4), &nodes[i].ip_port.port, sizeof(uint16_t));
            memcpy(data + packed_length + 1 + sizeof(IP4) + sizeof(uint16_t), nodes[i].client_id, EXT_PUBLIC_KEY);
            packed_length += size;
        } else if (ipv6 == 1) {
            uint32_t size = 1 + sizeof(IP6) + sizeof(uint16_t) + EXT_PUBLIC_KEY;

            if (packed_length + size > length)
                return -1;

            data[packed_length] = net_family;
            memcpy(data + packed_length + 1, &nodes[i].ip_port.ip.ip6, sizeof(IP6));
            memcpy(data + packed_length + 1 + sizeof(IP6), &nodes[i].ip_port.port, sizeof(uint16_t));
            memcpy(data + packed_length + 1 + sizeof(IP6) + sizeof(uint16_t), nodes[i].client_id, EXT_PUBLIC_KEY);
            packed_length += size;
        } else {
            return -1;
        }
    }

    return packed_length;
}

/* Unpack data of length into nodes of size max_num_nodes.
 * Put the length of the data processed in processed_data_len.
 * tcp_enabled sets if TCP nodes are expected (true) or not (false).
 *
 * return number of unpacked nodes on success.
 * return -1 on failure.
 */
int unpack_gca_nodes(GC_Announce_Node *nodes, uint16_t max_num_nodes, uint16_t *processed_data_len,
                     const uint8_t *data, uint16_t length, uint8_t tcp_enabled)
{
    uint32_t num = 0, len_processed = 0;

    while (num < max_num_nodes && len_processed < length) {
        int ipv6 = -1;
        uint8_t host_family;

        if (data[len_processed] == TOX_AF_INET) {
            ipv6 = 0;
            host_family = AF_INET;
        } else if (data[len_processed] == TOX_TCP_INET) {
            if (!tcp_enabled)
                return -1;

            ipv6 = 0;
            host_family = TCP_INET;
        } else if (data[len_processed] == TOX_AF_INET6) {
            ipv6 = 1;
            host_family = AF_INET6;
        } else if (data[len_processed] == TOX_TCP_INET6) {
            if (!tcp_enabled)
                return -2;

            ipv6 = 1;
            host_family = TCP_INET6;
        } else {
            return -3;
        }

        if (ipv6 == 0) {
            uint32_t size = 1 + sizeof(IP4) + sizeof(uint16_t) + EXT_PUBLIC_KEY;

            if (len_processed + size > length)
                return -4;

            nodes[num].ip_port.ip.family = host_family;
            memcpy(&nodes[num].ip_port.ip.ip4, data + len_processed + 1, sizeof(IP4));
            memcpy(&nodes[num].ip_port.port, data + len_processed + 1 + sizeof(IP4), sizeof(uint16_t));
            memcpy(nodes[num].client_id, data + len_processed + 1 + sizeof(IP4) + sizeof(uint16_t), EXT_PUBLIC_KEY);
            len_processed += size;
            ++num;
        } else if (ipv6 == 1) {
            uint32_t size = 1 + sizeof(IP6) + sizeof(uint16_t) + EXT_PUBLIC_KEY;

            if (len_processed + size > length)
                return -5;

            nodes[num].ip_port.ip.family = host_family;
            memcpy(&nodes[num].ip_port.ip.ip6, data + len_processed + 1, sizeof(IP6));
            memcpy(&nodes[num].ip_port.port, data + len_processed + 1 + sizeof(IP6), sizeof(uint16_t));
            memcpy(nodes[num].client_id, data + len_processed + 1 + sizeof(IP6) + sizeof(uint16_t), EXT_PUBLIC_KEY);
            len_processed += size;
            ++num;
        } else {
            return -6;
        }
    }

    if (processed_data_len)
        *processed_data_len = len_processed;

    return num;
}

/* Handle all decrypt procedures */
static int unwrap_gca_packet(const uint8_t *self_public_key, const uint8_t *self_secret_key, uint8_t *public_key,
                             uint8_t *data, uint8_t packet_type, const uint8_t *packet, uint16_t length)
{
    if (id_equal(packet + 1, self_public_key)) {
        fprintf(stderr, "announce unwrap failed: id_equal failed\n");
        return -1;
    }

    memcpy(public_key, packet + 1, ENC_PUBLIC_KEY);

    int header_len = GCA_HEADER_SIZE;
    uint8_t nonce[crypto_box_NONCEBYTES];

    if (packet_type == NET_PACKET_GCA_SEND_NODES) {
        header_len += RAND_ID_SIZE;
        memcpy(nonce, packet + 1 + ENC_PUBLIC_KEY + RAND_ID_SIZE, crypto_box_NONCEBYTES);
    } else if (packet_type == NET_PACKET_GCA_PING_REQUEST) {
        header_len += ENC_PUBLIC_KEY;
        memcpy(nonce, packet + 1 + ENC_PUBLIC_KEY + ENC_PUBLIC_KEY, crypto_box_NONCEBYTES);
    } else {
        memcpy(nonce, packet + 1 + ENC_PUBLIC_KEY, crypto_box_NONCEBYTES);
    }

    uint8_t plain[length - header_len - crypto_box_MACBYTES];
    int len = decrypt_data(public_key, self_secret_key, nonce, packet + header_len, length - header_len, plain);

    if (len != length - header_len - crypto_box_MACBYTES) {
        fprintf(stderr, "announce decrypt failed! len %d\n", len);
        return -1;
    }

    if (plain[0] != packet_type) {
        fprintf(stderr, "unwrap failed with wrong packet type (%d expected %d)\n", plain[0], packet_type);
        return -1;
    }

    memcpy(data, plain, len);
    return len;
}

/* Handle all encrypt procedures */
static int wrap_gca_packet(const uint8_t *send_public_key, const uint8_t *send_secret_key,
                           const uint8_t *recv_public_key, uint8_t *packet, const uint8_t *data,
                           uint32_t length, uint8_t packet_type)
{
    uint8_t nonce[crypto_box_NONCEBYTES];
    new_nonce(nonce);

    uint8_t encrypt[length + crypto_box_MACBYTES];
    int len = encrypt_data(recv_public_key, send_secret_key, nonce, data, length, encrypt);

    if (len != sizeof(encrypt)) {
        fprintf(stderr, "Announce encrypt failed\n");
        return -1;
    }

    packet[0] = packet_type;
    memcpy(packet + 1, send_public_key, ENC_PUBLIC_KEY);
    memcpy(packet + 1 + ENC_PUBLIC_KEY, nonce, crypto_box_NONCEBYTES);

    memcpy(packet + GCA_HEADER_SIZE, encrypt, len);
    return GCA_HEADER_SIZE + len;
}

static int add_gc_announced_node(GC_Announce *announce, const uint8_t *chat_id, const GC_Announce_Node node);

static int dispatch_packet_announce_request(GC_Announce* announce, Node_format *nodes, int nclosest,
                                            const uint8_t *chat_id, const uint8_t *sender_pk, const uint8_t *data,
                                            uint32_t length, bool self)
{
    uint8_t packet[length + GCA_HEADER_SIZE];
    int i, sent = 0;

    /* Relay announce request to all nclosest nodes if self announce */
    for (i = 0; i < nclosest; i++) {
        if (!self && id_closest(chat_id, nodes[i].client_id, sender_pk) != 1)
            continue;

        int packet_length = wrap_gca_packet(announce->dht->self_public_key, announce->dht->self_secret_key,
                                            nodes[i].client_id, packet, data, length, NET_PACKET_GCA_ANNOUNCE);

        if (packet_length == -1)
            continue;

        if (sendpacket(announce->dht->net, nodes[i].ip_port, packet, packet_length) != -1)
            ++sent;
    }

    /* Add to announcements if we're the closest node to chat_id and we aren't the announcer */
    if (sent == 0 && !self) {
        uint8_t chat_id[EXT_PUBLIC_KEY];
        memcpy(chat_id, data + 1, EXT_PUBLIC_KEY);

        GC_Announce_Node node;
        if (unpack_gca_nodes(&node, 1, 0, data + 1 + EXT_PUBLIC_KEY, length - 1 - EXT_PUBLIC_KEY, 0) != 1)
            return -1;

        add_gc_announced_node(announce, chat_id, node);
    }

    return sent;
}

static int dispatch_packet_get_nodes_request(GC_Announce* announce, Node_format *nodes, int nclosest,
                                             const uint8_t *chat_id, const uint8_t *sender_pk, const uint8_t *data,
                                             uint32_t length, bool self)
{
    uint8_t packet[length + GCA_HEADER_SIZE];
    int i, sent = 0;

    for (i = 0; i < nclosest; i++) {
        if (!self && id_closest(chat_id, nodes[i].client_id, sender_pk) != 1)
            continue;

        int packet_length = wrap_gca_packet(announce->dht->self_public_key, announce->dht->self_secret_key,
                                            nodes[i].client_id, packet, data, length, NET_PACKET_GCA_GET_NODES);
        if (packet_length == -1)
            continue;

        if (sendpacket(announce->dht->net, nodes[i].ip_port, packet, packet_length) != -1)
            ++sent;
    }

    return sent;
}

/* Returns the number of sent packets */
static int dispatch_packet(GC_Announce* announce, const uint8_t *chat_id, const uint8_t *sender_pk,
                           const uint8_t *data, uint32_t length, uint8_t packet_type, bool self)
{
    Node_format nodes[MAX_SENT_NODES];
    int nclosest = get_close_nodes(announce->dht, chat_id, nodes, 0, 1, 1);

    if (nclosest > MAX_GCA_SENT_NODES)
        nclosest = MAX_GCA_SENT_NODES;
    else if (nclosest == -1)
        return -1;

    if (packet_type == NET_PACKET_GCA_ANNOUNCE)
        return dispatch_packet_announce_request(announce, nodes, nclosest, chat_id, sender_pk, data, length, self);

    if (packet_type == NET_PACKET_GCA_GET_NODES)
        return dispatch_packet_get_nodes_request(announce, nodes, nclosest, chat_id, sender_pk, data, length, self);

    return -1;
}

/* Add requested online chat members to announce->self_requests
 *
 * Returns index of matching index on success.
 * Returns -1 on failure.
 */
static int add_requested_gc_nodes(GC_Announce *announce, const GC_Announce_Node *node, uint64_t req_id,
                                  uint32_t nodes_num)
{
    int i;
    uint32_t j;

    for (i = 0; i < MAX_GCA_SELF_REQUESTS; i++) {
        if (announce->self_requests[i].req_id != req_id)
            continue;

        for (j = 0; j < nodes_num; j++) {
            if (ipport_isset(&node[j].ip_port)
                && memcmp(announce->self_requests[i].long_pk, node[j].client_id, EXT_PUBLIC_KEY) != 0) {
                memcpy(announce->self_requests[i].nodes[j].client_id, node[j].client_id, EXT_PUBLIC_KEY);
                ipport_copy(&announce->self_requests[i].nodes[j].ip_port, &node[j].ip_port);
                announce->self_requests[i].ready = true;
            }
        }

        return i;
    }

    return -1;
}

static int add_announced_nodes_helper(GC_Announce *announce, const uint8_t *chat_id,
                                      const GC_Announce_Node node, int idx, bool update)
{
    uint64_t timestamp = unix_time();

    announce->announcements[idx].last_rcvd_ping = timestamp;
    announce->announcements[idx].last_sent_ping = timestamp;
    announce->announcements[idx].time_added = timestamp;
    ipport_copy(&announce->announcements[idx].node.ip_port, &node.ip_port);

    if (update)
        return idx;

    memcpy(announce->announcements[idx].node.client_id, node.client_id, EXT_PUBLIC_KEY);
    memcpy(announce->announcements[idx].chat_id, chat_id, EXT_PUBLIC_KEY);

    return idx;
}

/* Add announced node to announcements.
   If no slots are free replace the oldest node. */
static int add_gc_announced_node(GC_Announce *announce, const uint8_t *chat_id, const GC_Announce_Node node)
{
    int i, oldest_idx = 0;
    uint64_t oldest_announce = 0;

    for (i = 0; i < MAX_GCA_ANNOUNCED_NODES; i++) {
        if (oldest_announce < announce->announcements[i].time_added) {
            oldest_announce = announce->announcements[i].time_added;
            oldest_idx = i;
        }

        if (id_long_equal(announce->announcements[i].node.client_id, node.client_id)
            && id_long_equal(announce->announcements[i].chat_id, chat_id))
            return add_announced_nodes_helper(announce, chat_id, node, i, true);

        if (!ipport_isset(&announce->announcements[i].node.ip_port))
            return add_announced_nodes_helper(announce, chat_id, node, i, false);
    }

    return add_announced_nodes_helper(announce, chat_id, node, oldest_idx, false);
}

/* Gets up to MAX_GCA_SENT_NODES nodes that hold chat_id from announcements and add them to nodes array.
 * Returns the number of added nodes.
 */
static int get_gc_announced_nodes(GC_Announce *announce, const uint8_t *chat_id, GC_Announce_Node *nodes)
{
    int i, j = 0;

    for (i = 0; i < MAX_GCA_ANNOUNCED_NODES; i++) {
        if (!ipport_isset(&announce->announcements[i].node.ip_port))
            continue;

        if (id_long_equal(announce->announcements[i].chat_id, chat_id)) {
            memcpy(nodes[j].client_id, announce->announcements[i].node.client_id, EXT_PUBLIC_KEY);
            ipport_copy(&nodes[j].ip_port, &announce->announcements[i].node.ip_port);

            if (++j == MAX_GCA_SENT_NODES)
                break;
        }
    }

    return j;
}

/* Adds requested nodes that hold chat_id to self_requests.
 *
 * Returns array index on success.
 * Returns -1 on failure.
 */
static int add_announce_self_request(GC_Announce *announce, const uint8_t *chat_id, uint64_t req_id,
                                     const uint8_t *self_long_pk, const uint8_t *self_long_sk)
{
    int i;

    for (i = 0; i < MAX_GCA_SELF_REQUESTS; i++) {
        if (announce->self_requests[i].req_id == 0) {
            announce->self_requests[i].ready = 0;
            announce->self_requests[i].req_id = req_id;
            announce->self_requests[i].time_added = unix_time();
            memcpy(announce->self_requests[i].chat_id, chat_id, EXT_PUBLIC_KEY);
            memcpy(announce->self_requests[i].long_pk, self_long_pk, EXT_PUBLIC_KEY);
            memcpy(announce->self_requests[i].long_sk, self_long_sk, EXT_PUBLIC_KEY);
            return i;
        }
    }

    return -1;
}

/* Announce a new group chat */
int gca_send_announce_request(GC_Announce *announce, const uint8_t *self_long_pk, const uint8_t *self_long_sk,
                              const uint8_t *chat_id)
{
    DHT *dht = announce->dht;

    /* packet contains: type, chat_id, node, timestamp, signature */
    uint8_t data[1 + EXT_PUBLIC_KEY + sizeof(GC_Announce_Node) + TIME_STAMP_SIZE + SIGNATURE_SIZE];
    data[0] = NET_PACKET_GCA_ANNOUNCE;

    memcpy(data + 1, chat_id, EXT_PUBLIC_KEY);

    GC_Announce_Node self_node;
    if (make_self_gca_node(dht, &self_node, self_long_pk) == -1)
        return -1;

    int node_len = pack_gca_nodes(data + 1 + EXT_PUBLIC_KEY, sizeof(GC_Announce_Node), &self_node, 1);

    if (node_len <= 0) {
        fprintf(stderr, "pack_gca_nodes failed in gca_send_announce_request (%d)\n", node_len);
        return -1;
    }

    uint32_t length = 1 + EXT_PUBLIC_KEY + node_len + TIME_STAMP_SIZE + SIGNATURE_SIZE;
    uint8_t signed_data[length];

    if (sign_data(data, 1 + EXT_PUBLIC_KEY + node_len, self_long_sk, signed_data) == -1) {
        fprintf(stderr, "sign_data failed in gca_send_announce_request\n");
        return -1;
    }

    return dispatch_packet(announce, ENC_KEY(chat_id), dht->self_public_key, signed_data,
                           length, NET_PACKET_GCA_ANNOUNCE, true);
}

/* Attempts to relay an announce request to close nodes.
 * If we are the closest node store the node in announcements (this happens in dispatch_packet_announce_request)
 */
int handle_gca_request(void *ancp, IP_Port ipp, const uint8_t *packet, uint16_t length)
{
    if (length == 0 || length > MAX_GCA_PACKET_SIZE)
        return -1;

    GC_Announce* announce = ancp;
    DHT *dht = announce->dht;

    uint8_t data[length - GCA_HEADER_SIZE - crypto_box_MACBYTES];
    uint8_t public_key[ENC_PUBLIC_KEY];
    int plain_length = unwrap_gca_packet(dht->self_public_key, dht->self_secret_key, public_key, data,
                                         packet[0], packet, length);

    if (plain_length != length - GCA_HEADER_SIZE - crypto_box_MACBYTES) {
        fprintf(stderr, "unwrap failed in handle_gca_request (%d)\n", plain_length);
        return -1;
    }

    GC_Announce_Node node;
    if (unpack_gca_nodes(&node, 1, 0, data + 1 + EXT_PUBLIC_KEY, plain_length - 1 - EXT_PUBLIC_KEY, 0) != 1)
        return -1;

    if (crypto_sign_verify_detached(data + plain_length - SIGNATURE_SIZE, data,
                                    plain_length - SIGNATURE_SIZE,
                                    SIG_KEY(node.client_id)) != 0) {
        fprintf(stderr, "handle_gca_request sign verify failed\n");
        return -1;
    }

    return dispatch_packet(announce, ENC_KEY(data+1), dht->self_public_key, data,
                           plain_length, NET_PACKET_GCA_ANNOUNCE, false);
}

/* Sends a request for nodes that hold chat_id */
int gca_send_get_nodes_request(GC_Announce* announce, const uint8_t *self_long_pk, const uint8_t *self_long_sk,
                               const uint8_t *chat_id)
{
    DHT *dht = announce->dht;

    /* packet contains: type, chat_id, request_id, node, timestamp, signature */
    uint8_t data[1 + EXT_PUBLIC_KEY + RAND_ID_SIZE + sizeof(GC_Announce_Node) + TIME_STAMP_SIZE + SIGNATURE_SIZE];
    data[0] = NET_PACKET_GCA_GET_NODES;
    memcpy(data + 1, chat_id, EXT_PUBLIC_KEY);

    uint64_t request_id = random_64b();
    U64_to_bytes(data + 1 + EXT_PUBLIC_KEY, request_id);

    GC_Announce_Node self_node;
    if (make_self_gca_node(dht, &self_node, self_long_pk) == -1)
        return -1;

    int node_len = pack_gca_nodes(data + 1 + EXT_PUBLIC_KEY + RAND_ID_SIZE, sizeof(GC_Announce_Node), &self_node, 1);

    if (node_len <= 0) {
        fprintf(stderr, "pack_nodes failed in send_get_nodes_request\n");
        return -1;
    }

    uint32_t length = 1 + EXT_PUBLIC_KEY + RAND_ID_SIZE + node_len + TIME_STAMP_SIZE + SIGNATURE_SIZE;
    uint8_t sigdata[length];

    if (sign_data(data, 1 + EXT_PUBLIC_KEY + RAND_ID_SIZE + node_len, self_long_sk, sigdata) == -1) {
        fprintf(stderr, "gca_send_get_nodes_request sign_data failed\n");
        return -1;
    }

    add_announce_self_request(announce, ENC_KEY(chat_id), request_id, self_long_pk, self_long_sk);

    return dispatch_packet(announce, chat_id, dht->self_public_key, sigdata, length, NET_PACKET_GCA_GET_NODES, true);
}

/* Sends nodes that hold chat_id to node that requested them */
static int send_gca_get_nodes_response(DHT *dht, uint64_t request_id, IP_Port ipp, const uint8_t *receiver_pk,
                                       GC_Announce_Node *nodes, uint32_t num_nodes)
{
    /* packet contains: type, num_nodes, nodes, request_id */
    uint8_t data[1 + sizeof(uint32_t) + sizeof(GC_Announce_Node) * num_nodes + RAND_ID_SIZE];
    data[0] = NET_PACKET_GCA_SEND_NODES;
    U32_to_bytes(data + 1, num_nodes);

    int nodes_len = pack_gca_nodes(data + 1 + sizeof(uint32_t), sizeof(GC_Announce_Node) * num_nodes,
                                   nodes, num_nodes);
    if (nodes_len <= 0) {
        fprintf(stderr, "pack_gca_nodes failed in send_gca_get_nodes_response (%d)\n", nodes_len);
        return -1;
    }

    uint32_t plain_length = 1 + sizeof(uint32_t) + nodes_len + RAND_ID_SIZE;
    U64_to_bytes(data + plain_length - RAND_ID_SIZE, request_id);

    uint8_t packet[plain_length + RAND_ID_SIZE];
    int packet_length = wrap_gca_packet(dht->self_public_key, dht->self_secret_key, receiver_pk, packet, data,
                                        plain_length, NET_PACKET_GCA_SEND_NODES);
    if (packet_length == -1) {
        fprintf(stderr, "wrap failed in send_gca_get_nodes_response\n");
        return -1;
    }

    /* insert request_id into packet header after the packet type and dht_pk */
    memmove(packet + 1 + ENC_PUBLIC_KEY + RAND_ID_SIZE, packet + 1 + ENC_PUBLIC_KEY, packet_length - 1 - ENC_PUBLIC_KEY);
    U64_to_bytes(packet + 1 + ENC_PUBLIC_KEY, request_id);
    packet_length += RAND_ID_SIZE;

    return sendpacket(dht->net, ipp, packet, packet_length);
}

int handle_gc_get_announced_nodes_request(void *ancp, IP_Port ipp, const uint8_t *packet, uint16_t length)
{
    if (length == 0 || length > MAX_GCA_PACKET_SIZE)
        return -1;

    GC_Announce* announce = ancp;
    DHT *dht = announce->dht;

    uint8_t data[length - GCA_HEADER_SIZE - crypto_box_MACBYTES];
    uint8_t public_key[ENC_PUBLIC_KEY];
    int plain_length = unwrap_gca_packet(dht->self_public_key, dht->self_secret_key, public_key, data,
                                         packet[0], packet, length);

    if (plain_length != length - GCA_HEADER_SIZE - crypto_box_MACBYTES) {
        fprintf(stderr, "unwrap failed in handle_gc_get_announced_nodes_request %d\n", plain_length);
        return -1;
    }

    GC_Announce_Node node;
    if (unpack_gca_nodes(&node, 1, 0, data + 1 + EXT_PUBLIC_KEY + RAND_ID_SIZE,
                         plain_length - 1 - EXT_PUBLIC_KEY - RAND_ID_SIZE, 0) != 1) {
        fprintf(stderr, "unpack failed in handle_gc_get_announced_nodes_request\n");
        return -1;
    }

    if (crypto_sign_verify_detached(data + plain_length - SIGNATURE_SIZE,
                                    data, plain_length - SIGNATURE_SIZE,
                                    SIG_KEY(node.client_id)) != 0) {
        fprintf(stderr, "sign verify failed in handle announced nodes request\n");
        return -1;
    }

    GC_Announce_Node nodes[MAX_GCA_SENT_NODES];
    int num_nodes = get_gc_announced_nodes(announce, data + 1, nodes);

    if (num_nodes > 0) {
        uint64_t request_id;
        bytes_to_U64(&request_id, data + 1 + EXT_PUBLIC_KEY);

        return send_gca_get_nodes_response(dht, request_id, node.ip_port, ENC_KEY(node.client_id), nodes, num_nodes);
    }

    return dispatch_packet(announce, ENC_KEY(data+1), dht->self_public_key, data,
                           plain_length, NET_PACKET_GCA_GET_NODES, false);
}

int handle_gca_get_nodes_response(void *ancp, IP_Port ipp, const uint8_t *packet, uint16_t length)
{
    // NB: most probably we'll get nodes from different peers, so this would be called several times
    // TODO: different request_ids for the same chat_id... Probably

    if (length == 0 || length > MAX_GCA_PACKET_SIZE)
        return -1;

    GC_Announce *announce = ancp;
    DHT *dht = announce->dht;

    uint8_t data[length - GCA_HEADER_SIZE - crypto_box_MACBYTES - RAND_ID_SIZE];
    uint8_t public_key[ENC_PUBLIC_KEY];

    uint64_t request_id;
    bytes_to_U64(&request_id, packet + 1 + ENC_PUBLIC_KEY);

    int plain_length = 0;
    uint32_t i;

    for (i = 0; i < MAX_GCA_SELF_REQUESTS; i++) {
        if (announce->self_requests[i].req_id == request_id) {
            plain_length = unwrap_gca_packet(ENC_KEY(announce->self_requests[i].long_pk),
                                             ENC_KEY(announce->self_requests[i].long_sk),
                                             public_key, data, packet[0],packet, length);
            break;
        }
    }

    if (plain_length != length - GCA_HEADER_SIZE - crypto_box_MACBYTES - RAND_ID_SIZE) {
        fprintf(stderr, "unwrap failed in handle_gca_get_nodes_response %d\n", plain_length);
        return -1;
    }

    uint64_t request_id_enc;
    bytes_to_U64(&request_id_enc, data + plain_length - RAND_ID_SIZE);

    if (request_id != request_id_enc)
        return -1;

    uint32_t num_nodes;
    bytes_to_U32(&num_nodes, data + 1);

    /* this should never happen so assume it's malicious and ignore */
    if (num_nodes > MAX_GCA_SENT_NODES || num_nodes == 0)
        return -1;

    GC_Announce_Node nodes[num_nodes];
    int num_packed = unpack_gca_nodes(nodes, num_nodes, 0, data + 1 + sizeof(uint32_t),
                                      plain_length - 1 - sizeof(uint32_t), 0);

    if (num_packed != num_nodes) {
        fprintf(stderr, "unpack failed in handle_gca_get_nodes_response (got %d, expected %d)\n", num_packed, num_nodes);
        return -1;
    }

    if (add_requested_gc_nodes(announce, nodes, request_id, num_nodes) == -1)
        return -1;

    return 0;
}

/* Get group chat online members, which you searched for with get_announced_nodes_request */
int gca_get_requested_nodes(GC_Announce *announce, const uint8_t *chat_id, GC_Announce_Node *nodes)
{
    int i, j, k = 0;

    for (i = 0; i < MAX_GCA_SELF_REQUESTS; i++) {
        if (!id_long_equal(announce->self_requests[i].chat_id, chat_id))
            continue;

        if (! (announce->self_requests[i].ready == 1 && announce->self_requests[i].req_id != 0) )
            continue;

        for (j = 0; j < MAX_GCA_SENT_NODES; j++) {
            if (ipport_isset(&announce->self_requests[i].nodes[j].ip_port)) {
                memcpy(nodes[k].client_id, announce->self_requests[i].nodes[j].client_id, EXT_PUBLIC_KEY);
                ipport_copy(&nodes[k].ip_port, &announce->self_requests[i].nodes[j].ip_port);

                if (++k == MAX_GCA_SENT_NODES)
                    return k;
            }
        }
    }

    return k;
}

int handle_gca_ping_response(void *ancp, IP_Port ipp, const uint8_t *packet, uint16_t length)
{
    if (length != GCA_PING_RESPONSE_DHT_SIZE)
        return -1;

    GC_Announce *announce = ancp;
    DHT *dht = announce->dht;

    uint8_t data[GCA_PING_RESPONSE_PLAIN_SIZE];
    uint8_t public_key[ENC_PUBLIC_KEY];

    int plain_length = unwrap_gca_packet(dht->self_public_key, dht->self_secret_key, public_key, data,
                                         packet[0], packet, length);

    if (plain_length != GCA_PING_RESPONSE_PLAIN_SIZE)
        return -1;

    uint64_t ping_id;
    memcpy(&ping_id, data + 1, RAND_ID_SIZE);

    int i;

    for (i = 0; i < MAX_GCA_ANNOUNCED_NODES; ++i) {
        if (announce->announcements[i].ping_id == ping_id) {
            announce->announcements[i].ping_id = 0;

            if (!ipport_isset(&announce->announcements[i].node.ip_port))
                return -1;

            announce->announcements[i].last_rcvd_ping = unix_time();
            return 0;
        }
    }

    return -1;
}

static int send_gca_ping_response(DHT *dht, IP_Port ipp, const uint8_t *data, const uint8_t *rcv_pk)
{
    uint8_t response[GCA_PING_RESPONSE_PLAIN_SIZE];
    response[0] = NET_PACKET_GCA_PING_RESPONSE;
    memcpy(response + 1, data + 1, GCA_PING_RESPONSE_PLAIN_SIZE - 1);

    uint8_t packet[GCA_PING_RESPONSE_DHT_SIZE];
    int len = wrap_gca_packet(dht->self_public_key, dht->self_secret_key, rcv_pk, packet,
                              response, GCA_PING_RESPONSE_PLAIN_SIZE, NET_PACKET_GCA_PING_RESPONSE);
    if (len == -1)
        return -1;

    return sendpacket(dht->net, ipp, packet, len);
}

int handle_gca_ping_request(void *ancp, IP_Port ipp, const uint8_t *packet, uint16_t length)
{
    if (length != GCA_PING_REQUEST_DHT_SIZE)
        return -1;

    GC_Announce *announce = ancp;
    DHT *dht = announce->dht;

    uint8_t self_client_id[ENC_PUBLIC_KEY];
    memcpy(self_client_id, packet + 1 + ENC_PUBLIC_KEY, ENC_PUBLIC_KEY);

    int i;
    bool node_found = false;

    for (i = 0; i < MAX_GCA_SELF_REQUESTS; ++i) {
        if (memcmp(self_client_id, announce->self_requests[i].long_pk, ENC_PUBLIC_KEY) == 0) {
            node_found = true;
            break;
        }
    }

    if (!node_found) {
        fprintf(stderr, "handle ping request node not found\n");
        return -1;
    }

    uint8_t data[GCA_PING_REQUEST_PLAIN_SIZE];
    uint8_t public_key[ENC_PUBLIC_KEY];
    int plain_length = unwrap_gca_packet(dht->self_public_key, announce->self_requests[i].long_sk,
                                         public_key, data, packet[0], packet, length);

    if (plain_length != GCA_PING_REQUEST_PLAIN_SIZE) {
        fprintf(stderr, "handle ping request unwrap failed\n");
        return -1;
    }

    return send_gca_ping_response(dht, ipp, data, public_key);
}

static int gca_send_ping_request(DHT *dht, GC_Announce_Node *node, uint64_t ping_id)
{
    uint8_t data[GCA_PING_REQUEST_PLAIN_SIZE];
    data[0] = NET_PACKET_GCA_PING_REQUEST;
    memcpy(data + 1, &ping_id, RAND_ID_SIZE);

    uint8_t packet[GCA_PING_REQUEST_DHT_SIZE];
    int len = wrap_gca_packet(dht->self_public_key, dht->self_secret_key, node->client_id, packet, data,
                              GCA_PING_REQUEST_PLAIN_SIZE, NET_PACKET_GCA_PING_REQUEST);
    if (len == -1)
        return -1;

    /* insert recipient's client_id into packet header after the packet type and dht_pk */
    memmove(packet + 1 + ENC_PUBLIC_KEY + ENC_PUBLIC_KEY, packet + 1 + ENC_PUBLIC_KEY, len - 1 - ENC_PUBLIC_KEY);
    memcpy(packet + 1 + ENC_PUBLIC_KEY, node->client_id, ENC_PUBLIC_KEY);
    len += ENC_PUBLIC_KEY;

    return sendpacket(dht->net, node->ip_port, packet, len);
}

static void ping_gca_nodes(GC_Announce *announce)
{
    int i;

    for (i = 0; i < MAX_GCA_ANNOUNCED_NODES; ++i) {
        if (!ipport_isset(&announce->announcements[i].node.ip_port))
            continue;

        if (!is_timeout(announce->announcements[i].last_sent_ping, GCA_PING_INTERVAL))
            continue;

        uint64_t ping_id = random_64b();
        announce->announcements[i].ping_id = ping_id;
        gca_send_ping_request(announce->dht, &announce->announcements[i].node, ping_id);
        announce->announcements[i].last_sent_ping = unix_time();
    }
}

void do_gca(GC_Announce *announce)
{
    int i;

    for (i = 0; i < MAX_GCA_ANNOUNCED_NODES; i++) {
        if (!ipport_isset(&announce->announcements[i].node.ip_port))
            continue;

        if (is_timeout(announce->announcements[i].last_rcvd_ping, GCA_NODES_EXPIRATION)) {
            fprintf(stderr, "announce node %i timed out\n", i);
            memset(&announce->announcements[i], 0, sizeof(struct GC_AnnouncedNode));
        }
    }

    ping_gca_nodes(announce);
}

void gca_cleanup(GC_Announce *announce, const uint8_t *chat_id)
{
    int i;

    /* Remove all self_requests for chat_id */
    for (i = 0; i < MAX_GCA_SELF_REQUESTS; ++i) {
        if (! (announce->self_requests[i].ready && announce->self_requests[i].req_id > 0) )
            continue;

        if (id_long_equal(announce->self_requests[i].chat_id, chat_id))
            memset(&announce->self_requests[i], 0, sizeof(struct GC_AnnounceRequest));
    }
}

GC_Announce *new_gca(DHT *dht)
{
    GC_Announce *announce = calloc(1, sizeof(GC_Announce));

    if (announce == NULL)
        return NULL;

    announce->dht = dht;
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_ANNOUNCE, &handle_gca_request, announce);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_GET_NODES, &handle_gc_get_announced_nodes_request, announce);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_SEND_NODES, &handle_gca_get_nodes_response, announce);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_PING_REQUEST, &handle_gca_ping_request, announce);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_PING_RESPONSE, &handle_gca_ping_response, announce);
    return announce;
}

void kill_gca(GC_Announce *announce)
{
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_ANNOUNCE, NULL, NULL);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_GET_NODES, NULL, NULL);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_SEND_NODES, NULL, NULL);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_PING_REQUEST, NULL, NULL);
    networking_registerhandler(announce->dht->net, NET_PACKET_GCA_PING_RESPONSE, NULL, NULL);

    free(announce);
}
