/*
 * Copyright (c) 2014-2016 Alibaba Group. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */


#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "iot_import.h"
#include "CoAPExport.h"
#include "iot_import_coap.h"
#include "iot_import_dtls.h"
#include "CoAPNetwork.h"

#ifdef COAP_DTLS_SUPPORT
static void CoAPNetworkDTLS_freeSession (coap_remote_session_t *p_session);

unsigned int CoAPNetworkDTLS_read(coap_remote_session_t *p_session,
                                      unsigned char              *p_data,
                                      unsigned int               *p_datalen,
                                      unsigned int                timeout)
{
    unsigned int           err_code  = COAP_SUCCESS;
    const unsigned int     read_len  = *p_datalen;

    COAP_TRC("<< secure_datagram_read, read buffer len %d, timeout %d\r\n", read_len, timeout);
    if (NULL != p_session)
    {
        /* read dtls application data*/
        err_code = HAL_DTLSSession_read(p_session->context, p_data, p_datalen, timeout);
        if(DTLS_PEER_CLOSE_NOTIFY == err_code
                || DTLS_FATAL_ALERT_MESSAGE  == err_code) {
            COAP_INFO("dtls session read failed return (0x%04x)\r\n", err_code);
            CoAPNetworkDTLS_freeSession(p_session);
        }
    }

    COAP_TRC("<< secure_datagram_read process result (0x%04x)\r\n", err_code);

    return err_code;
}

unsigned int CoAPNetworkDTLS_write(coap_remote_session_t *p_session,
                                    const unsigned char        *p_data,
                                    unsigned int               *p_datalen)
{
    if(NULL != p_session){
        return HAL_DTLSSession_write(p_session->context, p_data, p_datalen);
    }
    return COAP_ERROR_INVALID_PARAM;
}

static void CoAPNetworkDTLS_initSession(coap_remote_session_t * p_session)
{
    memset(p_session, 0x00, sizeof(coap_remote_session_t));
    p_session->context = HAL_DTLSSession_init();
}

static  void CoAPNetworkDTLS_freeSession (coap_remote_session_t *p_session)
{
    /* Free the session.*/
    HAL_DTLSSession_free(p_session->context);
}

static unsigned int CoAPNetworkDTLS_createSession(int                        socket_id,
                                        const coap_address_t       *p_remote,
                                        unsigned char              *p_ca_cert_pem,
                                        coap_remote_session_t     *p_session,
                                        char *p_host)
{
    coap_dtls_options_t dtls_options;
    unsigned int err_code = COAP_SUCCESS;

    memset(&dtls_options, 0x00, sizeof(coap_dtls_options_t));
    dtls_options.p_ca_cert_pem     = p_ca_cert_pem;
    dtls_options.network.socket_id = socket_id;
    dtls_options.network.remote_port = p_remote->port;
    dtls_options.p_host            = p_host;
    memcpy(dtls_options.network.remote_addr, p_remote->addr, strlen(p_remote->addr));

    err_code = HAL_DTLSSession_create(p_session->context, &dtls_options);
    COAP_TRC("HAL_DTLSSession_create result %08x\r\n", err_code);

    if (COAP_SUCCESS != err_code)
    {
        CoAPNetworkDTLS_freeSession(p_session);
    }

    return err_code;
}

#endif

unsigned int CoAPNetwork_write(coap_network_t *p_network,
                                  const unsigned char  * p_data,
                                  unsigned int           datalen)
{
    int rc = COAP_ERROR_INTERNAL;

#ifdef COAP_DTLS_SUPPORT
    if(COAP_ENDPOINT_DTLS == p_network->ep_type){
        rc = CoAPNetworkDTLS_write(&p_network->remote_session, p_data, &datalen);
        COAP_DEBUG("[COAP-NWK]: >> Send secure message to %s:%d\r\n",
                                p_network->remote_endpoint.addr,
                                p_network->remote_endpoint.port);
    }
    else{
#endif
        COAP_DEBUG("[COAP-NWK]: >> Send nosecure message to %s:%d datalen: %d\r\n",
                   p_network->remote_endpoint.addr, p_network->remote_endpoint.port, datalen);
        rc = HAL_UDP_write((void *)&p_network->socket_id, &p_network->remote_endpoint, p_data, datalen);
        COAP_DEBUG("[CoAP-NWK]: Network write return %d\r\n", rc);

        if(-1 == rc) {
            rc = COAP_ERROR_INTERNAL;
        } else {
            rc = COAP_SUCCESS;
        }
#ifdef COAP_DTLS_SUPPORT
    }
#endif
    return (unsigned int)rc;
}

int CoAPNetwork_read(coap_network_t *network, unsigned char  *data,
                        unsigned int datalen, unsigned int timeout)
{
    unsigned int len = 0;

    #ifdef COAP_DTLS_SUPPORT
        if(COAP_ENDPOINT_DTLS == network->ep_type)  {
            len = datalen;
            memset(data, 0x00, datalen);
            CoAPNetworkDTLS_read(&network->remote_session, data, &len, timeout);
        } else {
    #endif
        memset(data, 0x00, datalen);
        len = HAL_UDP_readTimeout((void *)&network->socket_id,
                                  &network->remote_endpoint,
                                  data, COAP_MSG_MAX_PDU_LEN, timeout);
        COAP_DEBUG("<< CoAP recv nosecure data from %s:%d\r\n",
                 network->remote_endpoint.addr, network->remote_endpoint.port);
    #ifdef COAP_DTLS_SUPPORT
        }
    #endif
        COAP_TRC("<< CoAP recv %d bytes data\r\n", len);
        return len;
}

unsigned int CoAPNetwork_init(const coap_network_init_t *p_param, coap_network_t *p_network)
{
    unsigned int    err_code = COAP_SUCCESS;

    if(NULL == p_param || NULL == p_network){
        return COAP_ERROR_INVALID_PARAM;
    }

    /* TODO : Parse the url here */
    p_network->ep_type = p_param->ep_type;
    p_network->remote_endpoint.port = p_param->remote.port;
    memset(p_network->remote_endpoint.addr, 0x00, NETWORK_ADDR_LEN);
    memcpy(p_network->remote_endpoint.addr,  p_param->remote.addr, NETWORK_ADDR_LEN);

    /*Create udp socket*/
    HAL_UDP_create(&p_network->socket_id);

#ifdef COAP_DTLS_SUPPORT
    if(COAP_ENDPOINT_DTLS == p_param->ep_type){
        CoAPNetworkDTLS_initSession(&p_network->remote_session);
        err_code = CoAPNetworkDTLS_createSession(p_network->socket_id,
                    &p_param->remote, p_param->p_ca_cert_pem,
                    &p_network->remote_session, p_param->p_host);

    }
#endif
    return err_code;
}


unsigned int CoAPNetwork_deinit(coap_network_t *p_network)
{
    unsigned int    err_code = COAP_SUCCESS;
    HAL_UDP_close(&p_network->socket_id);
#ifdef COAP_DTLS_SUPPORT
    CoAPNetworkDTLS_freeSession(&p_network->remote_session);
#endif
    return err_code;
}

