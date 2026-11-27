/* P127545: passive H4 witness of the existing owner's commands only.
 * No command injection, no credit changes and no UART/FIFO operations.
 * Unknown/ambiguous framing invalidates the owner epoch, never qualifies.
 */
#ifndef HAKIOS_P127545_HCI_H
#define HAKIOS_P127545_HCI_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Transport marks describe the instant of TX submission / RX FIFO drain,
 * not delayed delivery. An attach/detach epoch crossing cannot count as IRQ
 * or polling evidence, even if the owner itself is unchanged. */
struct p545_hci_mark {
    uint64_t irq;
    uint32_t generation;
    bool attached,irq_pending,known;
};
struct p545_h4 {
    uint8_t type,hlen,hneed,h[4],first[4];
    uint32_t len,got;
    struct p545_hci_mark mark;
};
struct p545_hci_op {
    uint64_t tx,complete,irq,poll;
    struct p545_hci_mark mark;
    bool pending;
};
struct p545_hci {
    struct p545_h4 stream[2];
    struct p545_hci_op op[2];
    bool invalid;
};
static inline int p545_hci_opcode(uint16_t op){return op==0x1001?0:op==0x1003?1:-1;}
static inline void p545_hci_packet(struct p545_hci *s,struct p545_h4 *f,bool rx){
    int i=-1;
    if(!rx && f->type==1 && !f->len){
        i=p545_hci_opcode((uint16_t)(f->h[0]|f->h[1]<<8));
        if(i>=0){
            struct p545_hci_op *o=&s->op[i];
            if(o->pending){s->invalid=true;return;}
            o->tx++;o->pending=true;o->mark=f->mark;
        }
    }else if(rx && f->type==4 && f->h[0]==0x0e && f->len>=3){
        i=p545_hci_opcode((uint16_t)(f->first[1]|f->first[2]<<8));
        if(i>=0){
            struct p545_hci_op *o=&s->op[i];
            if(!o->pending){s->invalid=true;return;}
            o->pending=false;
            if(f->len==12 && f->first[3]==0){
                o->complete++;
                if(o->mark.known && f->mark.known &&
                   o->mark.generation==f->mark.generation &&
                   o->mark.attached==f->mark.attached){
                    if(f->mark.attached && f->mark.irq_pending && f->mark.irq>o->mark.irq)o->irq++;
                    if(!f->mark.attached && f->mark.irq==o->mark.irq)o->poll++;
                }
            }
        }
    }
}
static inline void p545_hci_feed(struct p545_hci *s,bool rx,const uint8_t *buf,size_t n,struct p545_hci_mark mark){
    struct p545_h4 *f=&s->stream[rx?1:0];
    for(size_t i=0;i<n && !s->invalid;i++){
        uint8_t c=buf[i];
        if(!f->type){
            if(c<1 || c>4 || (rx && c==1) || (!rx && c==4)){s->invalid=true;break;}
            f->type=c;f->hlen=0;f->got=0;f->hneed=c==2?4:c==4?2:3;f->mark=mark;
            continue;
        }
        if(!mark.known || f->mark.generation!=mark.generation || f->mark.attached!=mark.attached)
            f->mark.known=false;
        if(f->hlen<f->hneed){
            f->h[f->hlen++]=c;
            if(f->hlen<f->hneed)continue;
            f->len=f->type==2?(uint32_t)(f->h[2]|f->h[3]<<8):f->type==4?f->h[1]:f->h[2];
        }else{
            if(f->got<4)f->first[f->got]=c;
            f->got++;
        }
        if(f->got==f->len){p545_hci_packet(s,f,rx);f->type=0;}
    }
}
#endif
