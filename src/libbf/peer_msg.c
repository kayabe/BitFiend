#include "peer_msg.h"
#include "log.h"
#include "peer_id.h"
#include "peer_connection.h"
#include "lbitfield.h"
#include "piece_request.h"
#include "dl_file.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <time.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

int peer_send_buff(int sockfd, const char *buff, size_t len)
{
    ssize_t tot_sent = 0;
    while(tot_sent < len) {
        ssize_t sent = send(sockfd, buff, len - tot_sent, 0);
        if(sent < 0)
            return -1;

        tot_sent += sent;
        buff += sent;
    }
    return 0;
}

int peer_recv_buff(int sockfd, char *buff, size_t len)
{
    unsigned tot_recv = 0;
    ssize_t nb;

    if(len == 0)
        return 0;

    do {
        assert(len - tot_recv > 0);
        nb = recv(sockfd, buff + tot_recv, len - tot_recv, 0);
        if(nb < 0){
            return -1;
        }

        tot_recv += nb;

    }while(nb > 0 && tot_recv < len);

    if(tot_recv == len)
        return 0;
    else
        return -1;
}

int peer_recv_handshake(int sockfd, char outhash[20], char outpeerid[20], bool peer_id)
{
    const char *pstr = "BitTorrent protocol"; 
    unsigned char pstrlen = strlen(pstr);
    const char reserved[8] = {0};  

    size_t bufflen = 1 + pstrlen + sizeof(reserved) + 20
       + (peer_id ? sizeof(g_local_peer_id) : 0);

    char buff[bufflen];
    if(peer_recv_buff(sockfd, buff, bufflen))
        return -1;

    off_t off = 0;
    if(buff[off] != pstrlen)
        return -1;
    off++;
    if(strncmp(buff + off, pstr, pstrlen))
        return -1;
    off += pstrlen;

    /*Skip checking the reserved bits for now*/
    off += 8; 

    memcpy(outhash, buff + off, 20);            
    if(peer_id) {
        off += 20;
        memcpy(outpeerid, buff + off, sizeof(g_local_peer_id));
    }

    return 0;
}

int peer_send_handshake(int sockfd, char infohash[20])
{
    const char *pstr = "BitTorrent protocol"; 
    unsigned char pstrlen = strlen(pstr);
    const char reserved[8] = {0};  

    size_t bufflen = 1 + pstrlen + sizeof(reserved) + 20 + sizeof(g_local_peer_id);

    off_t off = 0;
    char buff[bufflen];

    buff[0] = pstrlen;
    off++;

    memcpy(buff + off, pstr, pstrlen);
    off += pstrlen;
    assert(off == 20);

    memcpy(buff + off, reserved, sizeof(reserved));
    off += sizeof(reserved);
    assert(off == 28);

    memcpy(buff + off, infohash, 20);

    off += 20;
    memcpy(buff + off, g_local_peer_id, sizeof(g_local_peer_id));
    
    return peer_send_buff(sockfd, buff, bufflen);
}

static uint32_t msgbuff_len(msg_type_t type, const torrent_t *torrent)
{
    uint32_t ret;
    switch(type){
        case MSG_KEEPALIVE:
            ret = 0;
            break;
        case MSG_PIECE:
            ret = 1 + 2 * sizeof(uint32_t) + PEER_REQUEST_SIZE;
            break;
        case MSG_BITFIELD:
            ret = 1 + LBITFIELD_NUM_BYTES(list_get_size(torrent->pieces));
            break;
        case MSG_REQUEST:
            ret = 1 + 3 * sizeof(uint32_t);
            break;
        case MSG_HAVE:
        case MSG_PORT:
            ret = 1 + sizeof(uint32_t);
            break;
        default:
            ret = 1;
    }
    return ret;
}

static inline bool valid_len(msg_type_t type, const torrent_t *torrent, uint32_t len)
{
    return (len == msgbuff_len(type, torrent));
}

static int peer_msg_recv_piece(int sockfd, peer_msg_t *out, const torrent_t *torrent, uint32_t len)
{
    log_printf(LOG_LEVEL_DEBUG, "*** peer_msg_recv_piece ***\n");
    uint32_t u32, left = len;
    
    if(peer_recv_buff(sockfd, (char*)&u32, sizeof(u32)))
        return -1;
    out->payload.piece.index = ntohl(u32);
    left -= sizeof(uint32_t);

    if(peer_recv_buff(sockfd, (char*)&u32, sizeof(u32)))
        return -1;
    out->payload.piece.begin = ntohl(u32);
    left -= sizeof(uint32_t);

    out->payload.piece.blocklen = left;
    piece_request_t *pr = piece_request_create(torrent, out->payload.piece.index);
    if(!pr)
        return -1;
    
    block_request_t *br = piece_request_block_at(pr, out->payload.piece.begin);
    assert(br);
    const unsigned char *entry;
    FOREACH_ENTRY(entry, br->filemems) {
        filemem_t mem = *(filemem_t*)entry;
        if(peer_recv_buff(sockfd, mem.mem, mem.size))
            goto fail_recv_piece;

    }
    piece_request_free(pr);
    return 0;

fail_recv_piece:
    piece_request_free(pr);
    return -1;
}

static int peer_msg_recv_pastlen(int sockfd, peer_msg_t *out, const torrent_t *torrent, uint32_t len)
{
    if(len == 0){
        out->type = MSG_KEEPALIVE;
        return 0;
    }

    unsigned char type;
    if(peer_recv_buff(sockfd, &type, 1))
        return -1;

    if(type >= MSG_MAX)
        return -1;

    if(!valid_len(type, torrent, len))
        return -1;

    out->type = type;
    unsigned left = len - 1;      

    switch(type){
        /* When we get a piece, write it to the mmap'd file directly */
        case MSG_PIECE:
        {
            assert(left > 0);
            if(peer_msg_recv_piece(sockfd, out, torrent, left))
                return -1;
            break;

        }
        case MSG_BITFIELD:
        {
            char buff[left];
            if(peer_recv_buff(sockfd, buff, left))
                return -1;

            out->payload.bitfield = byte_str_new(left, "");  
            if(!out->payload.bitfield)
                return -1;
            memcpy(out->payload.bitfield->str, buff, left);
            break;
        }
        case MSG_REQUEST:
        { 
            char buff[left];
            if(peer_recv_buff(sockfd, buff, left))
                return -1;

            assert(sizeof(buff) ==  4 * sizeof(uint32_t));
            uint32_t u32;  
            memcpy(&u32, buff, sizeof(uint32_t));
            out->payload.request.index= ntohl(u32);

            memcpy(&u32, buff + sizeof(uint32_t), sizeof(uint32_t));
            out->payload.request.begin= ntohl(u32);

            memcpy(&u32, buff + 2 * sizeof(uint32_t), sizeof(uint32_t));
            out->payload.request.length = ntohl(u32);
            break;
        }
        case MSG_HAVE:
        {
            uint32_t u32;  
            assert(left ==  sizeof(uint32_t));
            if(peer_recv_buff(sockfd, (char*)&u32, left))
                return -1;
            out->payload.have = ntohl(u32);
            break;
        }
        case MSG_PORT:
        {
            uint32_t u32;  
            assert(left ==  sizeof(uint32_t));
            if(peer_recv_buff(sockfd, (char*)&u32, left))
                return -1;
            out->payload.listen_port = ntohl(u32);
            break;
        }
        default:
            return -1;
    }
    
    log_printf(LOG_LEVEL_DEBUG, "Successfully received message from peer, Type: %hhu\n", type);
    return 0;      

}

static int peer_msg_send_piece(int sockfd, piece_msg_t *pmsg, const torrent_t *torrent)
{
    log_printf(LOG_LEVEL_DEBUG, "*** peer_msg_send_piece ***\n");
    piece_request_t *pr = piece_request_create(torrent, pmsg->index);
    if(!pr)
        return -1;

    size_t written = 0;
    off_t offset = 0;

    const unsigned char *entry;
    FOREACH_ENTRY(entry, pr->block_requests) {
        block_request_t *br = *(block_request_t**)entry;

        const unsigned char *fmem; 
        FOREACH_ENTRY(fmem, br->filemems) {
            filemem_t mem = *(filemem_t*)fmem;
            
            if(offset + mem.size > pmsg->begin) {
                size_t membegin = (offset < pmsg->begin) ? pmsg->begin - offset : 0;
                size_t memlen = MIN(mem.size - membegin, pmsg->blocklen - written); 

                if(peer_send_buff(sockfd, ((char*)mem.mem) + membegin, memlen))
                    goto fail_send_piece;

                written += memlen;
            }

            if(written == pmsg->blocklen)
                break;

            offset += mem.size;
        }
    }
    piece_request_free(pr);
    return 0;

fail_send_piece:
    piece_request_free(pr);
    return -1;
}

int peer_msg_send(int sockfd, peer_msg_t *msg, const torrent_t *torrent)
{
    uint32_t len = msgbuff_len(msg->type, torrent);
    len = htonl(len);

    log_printf(LOG_LEVEL_INFO, "Sending message of type: %d\n", msg->type);

    if(peer_send_buff(sockfd, (char*)&len, sizeof(uint32_t)))
        return -1;

    if(msg->type == MSG_KEEPALIVE)
        return 0;

    char out = msg->type;
    if(peer_send_buff(sockfd, &out, 1))
        return -1;

    switch(msg->type){
        MSG_CHOKE:
        MSG_UNCHOKE:
        MSG_INTERESTED:
        MSG_NOT_INTERESTED:
        MSG_CANCEL:
        {
            assert(len == 1);
            return 0;
        }
        case MSG_PIECE:
        {
            piece_msg_t *pmsg = &msg->payload.piece;
            return peer_msg_send_piece(sockfd, pmsg, torrent);
        }
        case MSG_BITFIELD:
        {
            assert(msg->payload.bitfield);
            if(peer_send_buff(sockfd, msg->payload.bitfield->str, msg->payload.bitfield->size))
                return -1;

            return 0;
        }
        case MSG_REQUEST:
        { 
            uint32_t u32;
            u32 = htonl(msg->payload.request.index);
            if(peer_send_buff(sockfd, (char*)&u32, sizeof(uint32_t)))
                return -1;
            u32 = htonl(msg->payload.request.begin);
            if(peer_send_buff(sockfd, (char*)&u32, sizeof(uint32_t)))
                return -1;
            u32 = htonl(msg->payload.request.length);
            if(peer_send_buff(sockfd, (char*)&u32, sizeof(uint32_t)))
                return -1;
        
            return 0;
        }
        case MSG_HAVE:
        {
            uint32_t u32;
            u32 = htonl(msg->payload.have);
            if(peer_send_buff(sockfd, (char*)&u32, sizeof(uint32_t)))
                return -1;            

            return 0;
        }
        case MSG_PORT:
        {
            uint32_t u32;
            u32 = htonl(msg->payload.listen_port);
            if(peer_send_buff(sockfd, (char*)&u32, sizeof(uint32_t)))
                return -1;            

            return 0;
        }
        default:
            return -1;
    }
}

int peer_msg_recv(int sockfd, peer_msg_t *out, const torrent_t *torrent)
{
    uint32_t len;
    if(peer_recv_buff(sockfd, (char*)&len, sizeof(uint32_t)))
        return -1;
    len = ntohl(len);

    return peer_msg_recv_pastlen(sockfd, out, torrent, len);
}

int peer_msg_waiton_recv(int sockfd, peer_msg_t *out, const torrent_t *torrent, unsigned timeout)
{
    struct timeval new, saved;
    new.tv_sec = timeout;
    new.tv_usec = 0;
    int old_cancelstate;
    uint32_t len;

    socklen_t timelen = sizeof(struct timeval);
    getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &saved, &timelen); 
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &new, sizeof(timeout));

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_cancelstate);
    /* The initial recv is a cancellation point, and has a custom timeout of "timeout" */
    ssize_t r; 
    if((r = recv(sockfd, &len, sizeof(uint32_t), 0)) <= 0)
        return -1;

    assert(r <= sizeof(uint32_t));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &saved, sizeof(struct timeval));

    pthread_setcancelstate(old_cancelstate, NULL);

    if(peer_recv_buff(sockfd, ((unsigned char*)&len) + r, sizeof(uint32_t) - r))
        return -1;

    len = ntohl(len);
    return peer_msg_recv_pastlen(sockfd, out, torrent, len);
}

bool peer_msg_buff_nonempty(int sockfd)
{
    uint32_t len;
    int n = recv(sockfd, (char*)&len, sizeof(uint32_t), MSG_PEEK | MSG_DONTWAIT);
    if(n < sizeof(uint32_t))
        return false;
    return true;    
}
