typedef uint8_t SwpSeqno;

typedef struct {
    SwpSeqno   SeqNum;   /* sequence number of this frame */
    SwpSeqno   AckNum;   /* ack of received frame */
    uint8_t     Flags;   /* up to 8 bits worth of flags */
} SwpHdr;

typedef struct {
    /* sender side state: */
    SwpSeqno    LAR;        /* seqno of last ACK received */
    SwpSeqno    LFS;        /* last frame sent */
    Semaphore   sendWindowNotFull;
    SwpHdr      hdr;        /* pre-initialized header */
    struct sendQ_slot {
        Event   timeout;    /* event associated with send-timeout */
        Msg     msg;
    }   sendQ[SWS];

    /* receiver side state: */
    SwpSeqno    NFE;       /* seqno of next frame expected */
    struct recvQ_slot {
        int     received;  /* is msg valid? */
        Msg     msg;
    }   recvQ[RWS];
} SwpState;

static int sendSWP(SwpState *state, Msg * frame)
{ 
    struct sendQ_slot * slot;
    char hbuf[HLEN];

    semWait(%state->sendWindowNotFull);
    state->hdr.SeqNum = ++state->LFS; 
    slot - &state->sendQ[state->hdr.SeqNum % SWS];
    store_swp_hdr(frame, hbuf, HLEN); 
    msgSaveCopy(&slot->msg, frame); 
    slot->timeout = evSchedule(swpTimeout, slot, SWP_SEND_TIMEOUT);
    return send(LINK, frame); 



}

static int deliverSWP(SWPState *state, Msg *frame)
{ 
    SwpHdr hdr; 
    char *hbuf; 

    hbuf = msgStripHdr(frame, HLEN): 
}