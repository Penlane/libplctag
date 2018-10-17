/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2013-11-19  KRH - Created file.                                        *
 **************************************************************************/

/*#ifdef __cplusplus
extern "C"
{
#endif
*/


#include <lib/libplctag.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/eip_pccc.h>
#include <ab/tag.h>
#include <ab/connection.h>
#include <ab/session.h>
#include <ab/defs.h>
#include <ab/eip.h>
#include <util/debug.h>


static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);

/*
 * ab_tag_status_pccc
 *
 * CIP-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int eip_pccc_tag_status(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int session_rc = PLCTAG_STATUS_OK;
    int connection_rc = PLCTAG_STATUS_OK;

    if(tag->read_in_progress) {
        return check_read_status(tag);
    }

    if(tag->write_in_progress) {
        return check_write_status(tag);
    }

    /* propagate the status up */
    if (tag->session) {
        session_rc = tag->session->status;
    } else {
        /* this is not OK.  This is fatal! */
        session_rc = PLCTAG_ERR_CREATE;
    }

    if(tag->needs_connection) {
        if(tag->connection) {
            connection_rc = tag->connection->status;
        } else {
            /* fatal! */
            connection_rc = PLCTAG_ERR_CREATE;
        }
    } else {
        connection_rc = PLCTAG_STATUS_OK;
    }

    /* now collect the status.  Highest level wins. */
    rc = session_rc;

    if(rc == PLCTAG_STATUS_OK) {
        rc = connection_rc;
    }

    if(rc == PLCTAG_STATUS_OK) {
        rc = tag->status;
    }

    return rc;
}


/*
 * eip_pccc_tag_read_start
 * Decide whether to start a standard or ucmm read
 */

int eip_pccc_tag_read_start(ab_tag_p tag)
{
	int rc = 0;
	if (tag->use_ucmm)
	{
		rc = eip_pccc_tag_read_start_ucmm(tag);
	}
	else
	{
		rc = eip_pccc_tag_read_start_standard(tag);
	}
	return rc;
}


/*
 * eip_pccc_tag_read_start_standard
 *
 * Start a PCCC tag read standard (PLC5, SLC).
 */

int eip_pccc_tag_read_start_standard(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));;
    int overhead;
    int data_per_packet;
    pccc_req *pccc;
    uint8_t *data;
    uint8_t *embed_start;

    pdebug(DEBUG_INFO,"Starting");

    /* how many packets will we need? How much overhead? */
    overhead = sizeof(pccc_resp) + 4 + tag->encoded_name_size; /* MAGIC 4 = fudge */

    /* calculate based on the response. */
    overhead =   1      /* reply code */
                +1      /* reserved */
                +1      /* general status */
                +1      /* status size */
                +1      /* request ID size */
                +2      /* vendor ID */
                +4      /* vendor serial number */
                +1      /* PCCC command */
                +1      /* PCCC status */
                +2      /* PCCC sequence number */
                +1      /* type byte */
                +2      /* maximum extended type. */
                +2      /* maximum extended size. */
                +1      /* secondary type byte if type was array. */
                +2      /* maximum extended type. */
                +2;     /* maximum extended size. */



    data_per_packet = MAX_PCCC_PACKET_SIZE - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_PCCC_PACKET_SIZE);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_DETAIL,"Tag size is %d, write overhead is %d, and write data per packet is %d.", tag->size, overhead, data_per_packet);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(!tag->reqs) {
        tag->reqs = (ab_request_p*)mem_alloc(1 * sizeof(ab_request_p));
        tag->max_requests = 1;
        tag->num_read_requests = 1;
        tag->num_write_requests = 1;

        if(!tag->reqs) {
            pdebug(DEBUG_WARN,"Unable to get memory for request array!");
            return PLCTAG_ERR_NO_MEM;
        }
    }

    /* get a request buffer */
    rc = request_create(&req, MAX_PCCC_PACKET_SIZE);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    req->num_retries_left = tag->num_retries;
    req->retry_interval = tag->default_retry_interval;

    /* point the struct pointers to the buffer*/
    pccc = (pccc_req*)(req->data);

    /* set up the embedded PCCC packet */
    embed_start = (uint8_t*)(&pccc->service_code);

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;  /* class */
    pccc->req_path[1] = 0x67;  /* PCCC Execute */
    pccc->req_path[2] = 0x24;  /* instance */
    pccc->req_path[3] = 0x01;  /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;  /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* fill in the PCCC command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id); /* FIXME - get sequence ID from session? */
    pccc->pccc_function = AB_EIP_PCCC_TYPED_READ_FUNC;
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */

    /* point to the end of the struct */
    data = ((uint8_t *)pccc) + sizeof(pccc_req);

    /* copy encoded tag name into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* we need the count twice? */
    *((uint16_le*)data) = h2le16(tag->elem_count); /* FIXME - bytes or INTs? */
    data += sizeof(uint16_le);

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_READ_RR_DATA);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    pccc->cpf_item_count        = h2le16(2);                /* ALWAYS 2 */
    pccc->cpf_nai_item_type     = h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
    pccc->cpf_nai_item_length   = h2le16(0);                /* ALWAYS 0 */
    pccc->cpf_udi_item_type     = h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */
    pccc->cpf_udi_item_length   = h2le16(data - embed_start);  /* REQ: fill in with length of remaining data. */

    /* set the size of the request */
    req->request_size = data - (req->data);

    /* mark it as ready to send */
    req->send_request = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
//        request_release(req);
        tag->reqs[0] = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;
    req = NULL;

    tag->read_in_progress = 1;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}

/*
 * eip_pccc_tag_read_start_ucmm
 *
 * Start a PCCC tag read ucmm (LGX).
 */

int eip_pccc_tag_read_start_ucmm(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));;
    int overhead;
    int data_per_packet;
    pccc_ucmm_req *pccc;
    uint8_t *data;
    uint8_t *embed_start;
    uint8_t *embed_end;
    uint8_t *cm_start;
    uint8_t *cm_end;

    pdebug(DEBUG_INFO,"Starting");

    /* how many packets will we need? How much overhead? */
    overhead = sizeof(pccc_resp) + 4 + tag->encoded_name_size; /* MAGIC 4 = fudge */

    /* calculate based on the response. */
    overhead =   1      /* reply code */
                +1      /* reserved */
                +1      /* general status */
                +1      /* status size */
                +1      /* request ID size */
                +2      /* vendor ID */
                +4      /* vendor serial number */
                +1      /* PCCC command */
                +1      /* PCCC status */
                +2      /* PCCC sequence number */
                +1      /* type byte */
                +2      /* maximum extended type. */
                +2      /* maximum extended size. */
                +1      /* secondary type byte if type was array. */
                +2      /* maximum extended type. */
                +2;     /* maximum extended size. */



    data_per_packet = MAX_PCCC_PACKET_SIZE - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_PCCC_PACKET_SIZE);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_DETAIL,"Tag size is %d, write overhead is %d, and write data per packet is %d.", tag->size, overhead, data_per_packet);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(!tag->reqs) {
        tag->reqs = (ab_request_p*)mem_alloc(1 * sizeof(ab_request_p));
        tag->max_requests = 1;
        tag->num_read_requests = 1;
        tag->num_write_requests = 1;

        if(!tag->reqs) {
            pdebug(DEBUG_WARN,"Unable to get memory for request array!");
            return PLCTAG_ERR_NO_MEM;
        }
    }

    /* get a request buffer */
    rc = request_create(&req, MAX_PCCC_PACKET_SIZE);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    req->num_retries_left = tag->num_retries;
    req->retry_interval = tag->default_retry_interval;

    /* point the struct pointers to the buffer*/
    pccc = (pccc_ucmm_req*)(req->data);

    /* set up the embedded PCCC packet */
    cm_start = (uint8_t*)(&pccc->cm_service_code);
    embed_start = (uint8_t*)(&pccc->service_code);

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;  /* class */
    pccc->req_path[1] = 0x67;  /* PCCC Execute */
    pccc->req_path[2] = 0x24;  /* instance */
    pccc->req_path[3] = 0x01;  /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;  /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* fill in the PCCC command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id); /* FIXME - get sequence ID from session? */
    pccc->pccc_function = AB_EIP_PCCC_TYPED_READ_FUNC;
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */

    /* point to the end of the struct */
    data = ((uint8_t *)pccc) + sizeof(pccc_ucmm_req);

    /* copy encoded tag name into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* we need the count twice? */
    *((uint16_le*)data) = h2le16(tag->elem_count); /* FIXME - bytes or INTs? */
    data += sizeof(uint16_le);

    embed_end = (uint8_t *)data;
    int diff = embed_end-embed_start;
    if (diff  % 2) // padding
    {
    	*data = 0x00;
    	data++;
    }

    cm_end = data+2*sizeof(uint16_le); // end of packet + routingpath

    /* We can now append the actual path*/
    *data = 0x01; // Route path size (words)
    data++;
    *data = tag->conn_path[1]; // CPU-Slot
    data++;
    *data = tag->conn_path[0]; // Backplane
    data++;

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_READ_RR_DATA);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    pccc->cpf_item_count        = h2le16(2);                /* ALWAYS 2 */
    pccc->cpf_nai_item_type     = h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
    pccc->cpf_nai_item_length   = h2le16(0);                /* ALWAYS 0 */
    pccc->cpf_udi_item_type     = h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */
    pccc->cpf_udi_item_length   = h2le16(cm_end - cm_start);  /* REQ: fill in with length of remaining data. */

    pccc->cm_service_code       = 0x52;
    pccc->cm_req_path_size      = 0x02;
    pccc->cm_req_path[0]        = 0x20;                         /* class */
    pccc->cm_req_path[1]        = 0x06;                         /* Connection Manager */
    pccc->cm_req_path[2]        = 0x24;                         /* instance */
    pccc->cm_req_path[3]        = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    pccc->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    pccc->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    pccc->uc_cmd_length = h2le16(embed_end - embed_start);

    /* set the size of the request */
    req->request_size = cm_end - (req->data);

    /* mark it as ready to send */
    req->send_request = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
//        request_release(req);
        tag->reqs[0] = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;
    req = NULL;

    tag->read_in_progress = 1;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */


static int check_read_status(ab_tag_p tag)
{
    pccc_resp *pccc;
    uint8_t *data;
    uint8_t *data_end;
    int pccc_res_type;
    int pccc_res_length;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_DETAIL,"Starting");

    /* PCCC only can have one request outstanding */
    /* is there an outstanding request? */
    if(!tag->reqs || !(tag->reqs[0])) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_WARN,"Read in progress but no requests in flight!");
        return PLCTAG_ERR_NULL_PTR;
    }

    req = tag->reqs[0];

    if(!req->resp_received) {
        return PLCTAG_STATUS_PENDING;
    }

    /* fake exceptions */
    do {
        pccc = (pccc_resp*)(req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        data_end = (req->data + le2h16(pccc->encap_length) + sizeof(eip_encap_t));

        if(le2h16(pccc->encap_command) != AB_EIP_READ_RR_DATA) {
            pdebug(DEBUG_WARN,"Unexpected EIP packet type received: %d!",pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"PCCC command failed, response code: %d",pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", *data, pccc_decode_error(*data));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(!(data = pccc_decode_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
            pdebug(DEBUG_WARN,"Unable to decode PCCC response data type and data size!");
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /* this gives us the overall type of the response and the number of bytes remaining in it.
         * If the type is an array, then we need to decode another one of these words
         * to get the type of each element and the size of each element.  We will
         * need to adjust the size if we care.
         */

        if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
            if(!(data = pccc_decode_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
                pdebug(DEBUG_WARN,"Unable to decode PCCC response array element data type and data size!");
                rc = PLCTAG_ERR_BAD_DATA;
                break;
            }
        }

        /* copy data into the tag. */
        if((data_end - data) > tag->size) {
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        mem_copy(tag->data, data, data_end - data);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    //session_remove_request(tag->session, req);
//    request_release(req);
    tag->reqs[0] = rc_dec(req);

    tag->read_in_progress = 0;

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}





/* FIXME  convert to unconnected messages. */

int eip_pccc_tag_write_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    pccc_req *pccc;
    uint8_t *data;
    uint8_t element_def[16];
    int element_def_size;
    uint8_t array_def[16];
    int array_def_size;
    int pccc_data_type;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));;
    ab_request_p req = NULL;
    uint8_t *embed_start;
    int overhead, data_per_packet;

    pdebug(DEBUG_INFO,"Starting.");

    /* how many packets will we need? How much overhead? */
    overhead = sizeof(pccc_resp) + 4 + tag->encoded_name_size; /* MAGIC 4 = fudge */

    /* overhead comes from the request in this case */
    overhead =   1  /* CIP PCCC command */
                +2  /* UCMM path size for PCCC command */
                +4  /* path to PCCC command object */
                +1  /* request ID size */
                +2  /* vendor ID */
                +4  /* vendor serial number */
                +1  /* PCCC command */
                +1  /* PCCC status */
                +2  /* PCCC sequence number */
                +1  /* PCCC function */
                +2  /* request offset */
                +2  /* request total transfer size in elements. */
                + (tag->encoded_name_size)
                +2; /* actual request size in elements */

    data_per_packet = MAX_PCCC_PACKET_SIZE - overhead;

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_PCCC_PACKET_SIZE);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        pdebug(DEBUG_DETAIL,"Tag size is %d, write overhead is %d, and write data per packet is %d.", MAX_PCCC_PACKET_SIZE, overhead, data_per_packet);
        return PLCTAG_ERR_TOO_LARGE;
    }


    /* set up the requests */
    if(!tag->reqs) {
        tag->reqs = (ab_request_p*)mem_alloc(1 * sizeof(ab_request_p));
        tag->max_requests = 1;
        tag->num_read_requests = 1;
        tag->num_write_requests = 1;

        if(!tag->reqs) {
            pdebug(DEBUG_WARN,"Unable to get memory for request array!");
            return PLCTAG_ERR_NO_MEM;
        }
    }

    /* get a request buffer */
    rc = request_create(&req, MAX_PCCC_PACKET_SIZE);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    req->num_retries_left = tag->num_retries;
    req->retry_interval = tag->default_retry_interval;

    pccc = (pccc_req*)(req->data);

    /* set up the embedded PCCC packet */
    embed_start = (uint8_t*)(&pccc->service_code);

    /* point to the end of the struct */
    data = (req->data) + sizeof(pccc_req);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* What type and size do we have? */
    if(tag->elem_size != 2 && tag->elem_size != 4) {
        pdebug(DEBUG_WARN,"Unsupported data type size: %d",tag->elem_size);
        //~ request_destroy(&req);
        rc_dec(req);
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    if(tag->elem_size == 4)
        pccc_data_type = AB_PCCC_DATA_REAL;
    else
        pccc_data_type = AB_PCCC_DATA_INT;

    /* generate the data type/data size fields, first the element part so that
     * we can get the size for the array part.
     */
    if(!(element_def_size = pccc_encode_dt_byte(element_def,sizeof(element_def),pccc_data_type,tag->elem_size))) {
        pdebug(DEBUG_WARN,"Unable to encode PCCC request array element data type and size fields!");
        //~ request_destroy(&req);
        rc_dec(req);
        return PLCTAG_ERR_ENCODE;
    }

    if(!(array_def_size = pccc_encode_dt_byte(array_def,sizeof(array_def),AB_PCCC_DATA_ARRAY,element_def_size + tag->size))) {
        pdebug(DEBUG_WARN,"Unable to encode PCCC request data type and size fields!");
        //~ request_destroy(&req);
        rc_dec(req);
        return PLCTAG_ERR_ENCODE;
    }

    /* copy the array data first. */
    mem_copy(data,array_def,array_def_size);
    data += array_def_size;

    /* copy the element data */
    mem_copy(data,element_def,element_def_size);
    data += element_def_size;

    /* now copy the data to write */
    mem_copy(data,tag->data,tag->size);
    data += tag->size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_READ_RR_DATA);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    pccc->cpf_item_count        = h2le16(2);                /* ALWAYS 2 */
    pccc->cpf_nai_item_type     = h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
    pccc->cpf_nai_item_length   = h2le16(0);                /* ALWAYS 0 */
    pccc->cpf_udi_item_type     = h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */
    pccc->cpf_udi_item_length   = h2le16(data - embed_start);  /* REQ: fill in with length of remaining data. */

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;  /* class */
    pccc->req_path[1] = 0x67;  /* PCCC Execute */
    pccc->req_path[2] = 0x24;  /* instance */
    pccc->req_path[3] = 0x01;  /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;  /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    //pccc->pccc_seq_num = h2le16(tag->connection->conn_seq_num);
    pccc->pccc_function = AB_EIP_PCCC_TYPED_WRITE_FUNC;
    /* FIXME - what should be the count here?  It is bytes, 16-bit
     * words or something else?
     *
     * Seems to be the number of elements??
     */
    pccc->pccc_transfer_size = h2le16(tag->elem_count); /* This is not in the docs, but it is in the data. */


    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;
    req->conn_seq = conn_seq_id;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
//        request_release(req);
        tag->reqs[0] = rc_dec(req);
        return rc;
   }

    /* save the request for later */
    tag->reqs[0] = req;

    /* the write is now pending */
    tag->write_in_progress = 1;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



/*
 * check_write_status
 *
 * Fragments are not supported.
 */
static int check_write_status(ab_tag_p tag)
{
    pccc_resp *pccc;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_DETAIL,"Starting.");

    /* is there an outstanding request? */
    if(!tag->reqs || !(tag->reqs[0]) ) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN,"Write in progress but not requests in flight!");
        return PLCTAG_ERR_NULL_PTR;
    }

    req = tag->reqs[0];

    if(!req->resp_received) {
        return PLCTAG_STATUS_PENDING;
    }

    /* fake exception */
    do {
        pccc = (pccc_resp*)(req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        /* check the response status */
        if( le2h16(pccc->encap_command) != AB_EIP_READ_RR_DATA) {
            pdebug(DEBUG_WARN,"EIP unexpected response packet type: %d!",pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"PCCC command failed, response code: %d",pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s",pccc->pccc_status, pccc_decode_error(*data));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    //session_remove_request(tag->session, req);
//    request_release(req);
    tag->reqs[0] = rc_dec(req);

    tag->write_in_progress = 0;

    pdebug(DEBUG_WARN,"Done.");

    /* Success! */
    return rc;
}
