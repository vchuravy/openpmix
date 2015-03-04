/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015      Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/util/error.h"

#include "pmix_server_ops.h"

// local functions
static void op_cbfunc(int status, void *cbdata);
static int server_switchyard(pmix_server_caddy_t *cd,
                             pmix_buffer_t *buf);
static void snd_message(int sd, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *msg, void *srv_obj,
                        pmix_send_message_cbfunc_t snd);

size_t PMIx_message_hdr_size(void)
{
    return sizeof(pmix_usock_hdr_t);
}

size_t PMIx_message_payload_size(char *readhdr)
{
    pmix_usock_hdr_t *hdr = (pmix_usock_hdr_t*)readhdr;
    return hdr->nbytes;
}

int PMIx_server_authenticate_client(int sd, int *rank, pmix_send_message_cbfunc_t snd_msg)
{
    int rc;
    pmix_peer_t *peer;
    pmix_buffer_t *info, reply;
    pmix_usock_hdr_t hdr;
    
    /* perform the authentication */
    if (PMIX_SUCCESS != (rc = pmix_server_authenticate(sd, rank, &peer, &info))) {
        return rc;
    }

    if (NULL != snd_msg) {
        /* pack the response, starting with rc */
        PMIX_CONSTRUCT(&reply, pmix_buffer_t);
        (void)pmix_bfrop.pack(&reply, &rc, 1, PMIX_INT);
        if (NULL != info) {
            /* transfer the data payload */
            pmix_bfrop.copy_payload(&reply, info);
            PMIX_RELEASE(info);
        }
        /* create the header */
        hdr.pindex = peer->index;  // pass back the index so the client can include it in future msgs
        hdr.nbytes = reply.bytes_used;
        hdr.tag = 0; // tag doesn't matter as we aren't matching to a recv

        /* send the response */
        snd_message(sd, &hdr, &reply, NULL, snd_msg);
        /* protect the data */
        reply.base_ptr = NULL;
        PMIX_DESTRUCT(&reply);
    }

    return rc;
}

int PMIx_server_process_msg(int sd, char *hdrptr, char *msgptr,
                            pmix_send_message_cbfunc_t snd_msg)
{
    pmix_usock_hdr_t *hdr = (pmix_usock_hdr_t*)hdrptr;
    pmix_buffer_t buf;
    pmix_server_caddy_t *cd;
    int rc;
    pmix_peer_t *peer;
    
    /* get the peer object for this client - the header
     * contains the index to it */
    if (NULL == (peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, hdr->pindex))) {
        return PMIX_ERR_NOT_FOUND;
    }
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "PMIx_server_process_msg for %s:%d:%d",
                        peer->info->nptr->nspace, peer->info->rank, sd);

    /* Load payload into the buffer */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER(&buf, msgptr, hdr->nbytes);

    /* setup the caddy */
    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(peer);
    cd->peer = peer;
    cd->snd.sd = sd;
    (void)memcpy(&cd->hdr, hdr, sizeof(pmix_usock_hdr_t));
    cd->snd.cbfunc = snd_msg;
    
    /* process the message */
    rc = server_switchyard(cd, &buf);
    PMIX_RELEASE(cd);
    
    /* Free buffer protecting the data */
    buf.base_ptr = NULL;
    PMIX_DESTRUCT(&buf);

    return rc;
}

static void get_cbfunc(int status, pmix_modex_data_t data[],
                       size_t ndata, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    int rc;
    pmix_buffer_t reply;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "serverlite:get_cbfunc called with %d elements", (int)ndata);

    if (NULL == cd) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    PMIX_CONSTRUCT(&reply, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* pack the nblobs being returned */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &ndata, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < ndata) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, data, ndata, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* send the data to the requestor */
    snd_message(cd->snd.sd, &cd->hdr, &reply, cd->peer->info->server_object, cd->snd.cbfunc);
    reply.base_ptr = NULL;

 cleanup:
    /* protect the data */
    PMIX_DESTRUCT(&reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}


static void spawn_cbfunc(int status, char *nspace, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    int rc;
    pmix_buffer_t reply;
    
    /* setup the reply with the returned status */
    PMIX_CONSTRUCT(&reply, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* add the nspace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* send the result */
    snd_message(cd->snd.sd, &cd->hdr, &reply, cd->peer->info->server_object, cd->snd.cbfunc);
    /* protect the data */
    reply.base_ptr = NULL;

 cleanup:
    PMIX_DESTRUCT(&reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}

static void lookup_cbfunc(int status, pmix_info_t info[], size_t ninfo,
                          char nspace[], void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    int rc;
    pmix_buffer_t reply;
    
    /* setup the reply with the returned status */
    PMIX_CONSTRUCT(&reply, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* pack the returned nspace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* pack the returned info objects */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* send to the originator */
    snd_message(cd->snd.sd, &cd->hdr, &reply, cd->peer->info->server_object, cd->snd.cbfunc);
    /* protect the data */
    reply.base_ptr = NULL;

 cleanup:
    PMIX_DESTRUCT(&reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}

static void modex_cbfunc(int status, pmix_modex_data_t data[],
                         size_t ndata, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    int rc;
    pmix_server_caddy_t *cd;
    pmix_buffer_t reply, rmsg;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "serverlite:modex_cbfunc called with %d elements", (int)ndata);

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    PMIX_CONSTRUCT(&reply, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&reply);
        return;
    }
    /* pack the nblobs being returned */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &ndata, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&reply);
        return;
    }
    if (0 < ndata) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, data, ndata, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&reply);
            return;
        }
    }
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        PMIX_CONSTRUCT(&rmsg, pmix_buffer_t);
        pmix_bfrop.copy_payload(&rmsg, &reply);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "serverlite:modex_cbfunc reply being sent to %s:%d",
                            cd->peer->info->nptr->nspace, cd->peer->info->rank);
        /* send the message */
        snd_message(cd->snd.sd, &cd->hdr, &rmsg, cd->peer->info->server_object, cd->snd.cbfunc);
        /* protect the data */
        rmsg.base_ptr = NULL;
        PMIX_DESTRUCT(&rmsg);
    }
    PMIX_DESTRUCT(&reply);
    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);
}

static void op_cbfunc(int status, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    int rc;
    pmix_buffer_t reply;
    
    /* setup the reply with the returned status */
    PMIX_CONSTRUCT(&reply, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* send the result */
    snd_message(cd->snd.sd, &cd->hdr, &reply, cd->peer->info->server_object, cd->snd.cbfunc);
    /* protect the data */
    reply.base_ptr = NULL;

 cleanup:
    PMIX_DESTRUCT(&reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}

static void cnct_cbfunc(int status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    int rc;
    pmix_server_caddy_t *cd;
    pmix_buffer_t reply, rmsg;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "serverlite:cnct_cbfunc called");

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        PMIX_CONSTRUCT(&rmsg, pmix_buffer_t);
        pmix_bfrop.copy_payload(&rmsg, &reply);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "serverlite:cnct_cbfunc reply being sent to %s:%d",
                            cd->peer->info->nptr->nspace, cd->peer->info->rank);
        /* send the message */
        snd_message(cd->snd.sd, &cd->hdr, &rmsg, cd->peer->info->server_object, cd->snd.cbfunc);
        /* protect the data */
        rmsg.base_ptr = NULL;
        PMIX_DESTRUCT(&rmsg);
    }
    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);
}


static int server_switchyard(pmix_server_caddy_t *cd,
                             pmix_buffer_t *buf)
{
    int rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    
    /* retrieve the cmd */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
        op_cbfunc(rc, cd);
        return rc;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd pmix cmd %d from %s:%d",
                        cmd, cd->peer->info->nptr->nspace, cd->peer->info->rank);


    if (PMIX_ABORT_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_abort(cd->peer, buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }
        
    if (PMIX_FENCENB_CMD == cmd) {
        if (PMIX_SUCCESS != (rc = pmix_server_fence(cd, buf, modex_cbfunc, op_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

    if (PMIX_GETNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_get(buf, get_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }
        
    if (PMIX_FINALIZE_CMD == cmd) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FINALIZE");
        /* call the local server, if supported */
        if (NULL != pmix_host_server.finalized) {
            PMIX_RETAIN(cd);
            if (PMIX_SUCCESS != (rc = pmix_host_server.finalized(cd->peer->info->nptr->nspace,
                                                                 cd->peer->info->rank,
                                                                 cd->peer->info->server_object,
                                                                 op_cbfunc, cd))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(cd);
            }
        } else {
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

        
    if (PMIX_PUBLISHNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_publish(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

    
    if (PMIX_LOOKUPNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_lookup(buf, lookup_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

        
    if (PMIX_UNPUBLISHNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_unpublish(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

        
    if (PMIX_SPAWNNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_spawn(buf, spawn_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

        
    if (PMIX_CONNECTNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, false, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

    if (PMIX_DISCONNECTNB_CMD == cmd) {
        PMIX_RETAIN(cd);
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, true, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RETAIN(cd); // op_cbfunc will release it to maintain accounting
            op_cbfunc(rc, cd);
        }
        return rc;
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

static void snd_message(int sd, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *msg, void *srv_obj,
                        pmix_send_message_cbfunc_t snd)
{
    size_t rsize;
    char *rmsg;

    /* setup the reply */
    rsize = sizeof(pmix_usock_hdr_t) + msg->bytes_used;
    rmsg = (char*)malloc(rsize);
    hdr->nbytes = msg->bytes_used;
    /* copy the header */
    memcpy(rmsg, hdr, sizeof(pmix_usock_hdr_t));
    /* add in the reply bytes */
    (void)memcpy(rmsg+sizeof(pmix_usock_hdr_t), msg->base_ptr, msg->bytes_used);
    /* send it - the send function will release the rmsg storage */
    snd(sd, srv_obj, rmsg, rsize);
}

