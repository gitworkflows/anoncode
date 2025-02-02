
#include "or.h"

/* 
 *
 * these two functions are the main ways 'in' to connection_or
 *
 */

int connection_or_process_inbuf(connection_t *conn) {

  assert(conn && conn->type == CONN_TYPE_OR);

  if(conn->inbuf_reached_eof) {
    /* eof reached, kill it. */
    log(LOG_DEBUG,"connection_or_process_inbuf(): conn reached eof. Closing.");
    return -1;
  }

  log(LOG_DEBUG,"connection_or_process_inbuf(): state %d.",conn->state);

  switch(conn->state) {
    case OR_CONN_STATE_CLIENT_AUTH_WAIT:
      return or_handshake_client_process_auth(conn);
    case OR_CONN_STATE_SERVER_AUTH_WAIT:
      return or_handshake_server_process_auth(conn);
    case OR_CONN_STATE_SERVER_NONCE_WAIT:
      return or_handshake_server_process_nonce(conn);
    case OR_CONN_STATE_OPEN:
      return connection_process_cell_from_inbuf(conn);
    default:
      log(LOG_DEBUG,"connection_or_process_inbuf() called in state where I'm writing. Ignoring buf for now.");
  }

  return 0;

}

int connection_or_finished_flushing(connection_t *conn) {
  int e, len=sizeof(e);

  assert(conn && conn->type == CONN_TYPE_OR);

  switch(conn->state) {
    case OR_CONN_STATE_CLIENT_CONNECTING:
      if (getsockopt(conn->s, SOL_SOCKET, SO_ERROR, &e, &len) < 0)  { /* not yet */
        if(errno != EINPROGRESS){
          /* yuck. kill it. */
          log(LOG_DEBUG,"connection_or_finished_flushing(): in-progress connect failed. Removing.");	
          return -1;
        } else {
          return 0; /* no change, see if next time is better */
        }
      }
      /* the connect has finished. */

      log(LOG_DEBUG,"connection_or_finished_flushing() : Connection to router %s:%u established.",
          conn->address,ntohs(conn->port));

      return or_handshake_client_send_auth(conn);
    case OR_CONN_STATE_CLIENT_SENDING_AUTH:
      log(LOG_DEBUG,"connection_or_finished_flushing(): client finished sending auth.");
      conn->state = OR_CONN_STATE_CLIENT_AUTH_WAIT;
      connection_watch_events(conn, POLLIN);
      return 0;
    case OR_CONN_STATE_CLIENT_SENDING_NONCE:
      log(LOG_DEBUG,"connection_or_finished_flushing(): client finished sending nonce.");
      conn_or_init_crypto(conn);
      conn->state = OR_CONN_STATE_OPEN;
      connection_watch_events(conn, POLLIN);
      return 0;
    case OR_CONN_STATE_SERVER_SENDING_AUTH:
      log(LOG_DEBUG,"connection_or_finished_flushing(): server finished sending auth.");
      conn->state = OR_CONN_STATE_SERVER_NONCE_WAIT;
      connection_watch_events(conn, POLLIN);
      return 0;
    case OR_CONN_STATE_OPEN:
      /* FIXME down the road, we'll clear out circuits that are pending to close */
      connection_watch_events(conn, POLLIN);
      return 0;
    default:
      log(LOG_DEBUG,"Bug: connection_or_finished_flushing() called in unexpected state.");
      return 0;
  }

  return 0;

}

/*********************/

connection_t *connection_or_new(void) {
  return connection_new(CONN_TYPE_OR);
}

connection_t *connection_or_new_listener(void) {
  return connection_new(CONN_TYPE_OR_LISTENER);
}

void conn_or_init_crypto(connection_t *conn) {
  int x;

  assert(conn);
  printf("f_session_key: ");
  for(x=0;x<8;x++) {
    printf("%d ",conn->f_session_key[x]);
  }
  printf("\nb_session_key: ");
  for(x=0;x<8;x++) {
    printf("%d ",conn->b_session_key[x]);
  }
  printf("\n");

  memset(conn->f_session_iv,0,8);
  memset(conn->b_session_iv,0,8);
  EVP_CIPHER_CTX_init(&conn->f_ctx);
  EVP_CIPHER_CTX_init(&conn->b_ctx);
  EVP_EncryptInit(&conn->f_ctx, EVP_des_ofb(), conn->f_session_key, conn->f_session_iv);
  EVP_DecryptInit(&conn->b_ctx, EVP_des_ofb(), conn->b_session_key, conn->b_session_iv);
    /* always encrypt with f, always decrypt with b */
  
}

/*
 *
 * auth handshake, as performed by OR *initiating* the connection
 *
 */

int connect_to_router(routerinfo_t *router, RSA *prkey, struct sockaddr_in *local) {
  connection_t *conn;
  struct sockaddr_in router_addr;
  int s;

  if ((!router) || (!prkey) || (!local))
    return -1;

  if(router->addr == local->sin_addr.s_addr && router->port == local->sin_port) {
    /* this is me! don't connect to me. */
    return 0;
  }

  conn = connection_or_new();

  /* set up conn so it's got all the data we need to remember */
  conn->addr = router->addr, conn->port = router->port;
  conn->prkey = prkey;
  conn->min = router->min, conn->max = router->max;
  conn->pkey = router->pkey;
  conn->address = strdup(router->address);
  memcpy(&conn->local,local,sizeof(struct sockaddr_in));

  s=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0)
  { 
    log(LOG_ERR,"Error creating network socket.");
    connection_free(conn);
    return -1;
  }
  fcntl(s, F_SETFL, O_NONBLOCK); /* set s to non-blocking */

  memset((void *)&router_addr,0,sizeof(router_addr));
  router_addr.sin_family = AF_INET;
  router_addr.sin_port = router->port;
  memcpy((void *)&router_addr.sin_addr, &router->addr, sizeof(uint32_t));

  log(LOG_DEBUG,"connect_to_router() : Trying to connect to %s:%u.",inet_ntoa(*(struct in_addr *)&router->addr),ntohs(router->port));

  if(connect(s,(struct sockaddr *)&router_addr,sizeof(router_addr)) < 0){
    if(errno != EINPROGRESS){
      /* yuck. kill it. */
      connection_free(conn);
      return -1;
    } else {
      /* it's in progress. set state appropriately and return. */
      conn->s = s;

      conn->state = OR_CONN_STATE_CLIENT_CONNECTING;

      if(connection_add(conn) < 0) { /* no space, forget it */
        connection_free(conn);
        return -1;
      }
      /* i think only pollout is needed, but i'm curious if pollin ever gets caught -RD */
      log(LOG_DEBUG,"connect_to_router() : connect in progress.");
      connection_watch_events(conn, POLLOUT | POLLIN);
      return 0;
    }
  }

  /* it succeeded. we're connected. */
  conn->s = s;

  if(connection_add(conn) < 0) { /* no space, forget it */
    connection_free(conn);
    return -1;
  }

  log(LOG_DEBUG,"connect_to_router() : Connection to router %s:%u established.",router->address,ntohs(router->port));

  /* move to the next step in the handshake */
  if(or_handshake_client_send_auth(conn) < 0) {
    connection_remove(conn);
    connection_free(conn);
    return -1;
  }
  return 0; 
}

int or_handshake_client_send_auth(connection_t *conn) {
  int retval;
  char keys[16];
  char buf[44];
  char cipher[128];

  if (!conn)
    return -1;

  /* generate random keys */
  retval = RAND_bytes(keys,16);
  if (retval != 1) /* error */
  { 
    log(LOG_ERR,"Cannot generate a secure DES key.");
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_client_send_auth() : Generated DES keys.");

  /* save keys */
  memcpy(conn->f_session_key,keys,8);
  memcpy(conn->b_session_key,keys+8,8);

  /* generate first message */
  memcpy(buf,&conn->local.sin_addr,4); /* local address */
  memcpy(buf+4,(void *)&conn->local.sin_port,2); /* local port */
  memcpy(buf+6, (void *)&conn->addr, 4); /* remote address */
  memcpy(buf+10, (void *)&conn->port, 2); /* remote port */
  memcpy(buf+12,keys,16); /* keys */
  *((uint32_t *)(buf+28)) = htonl(conn->min); /* min link utilisation */
  *((uint32_t *)(buf+32)) = htonl(conn->max); /* maximum link utilisation */
  log(LOG_DEBUG,"or_handshake_client_send_auth() : Generated first authentication message.");

  /* encrypt message */
  retval = RSA_public_encrypt(36,buf,cipher,conn->pkey,RSA_PKCS1_PADDING);
  if (retval == -1) /* error */
  { 
    log(LOG_ERR,"Public-key encryption failed during authentication to %s:%u.",conn->address,ntohs(conn->port));
    log(LOG_DEBUG,"or_handshake_client_send_auth() : Reason : %s.",ERR_reason_error_string(ERR_get_error()));
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_client_send_auth() : Encrypted authentication message.");

  /* send message */
  
  if(connection_write_to_buf(cipher, 128, conn) < 0) {
    log(LOG_DEBUG,"or_handshake_client_send_auth(): my outbuf is full. Oops.");
    return -1;
  }
  retval = connection_flush_buf(conn);
  if(retval < 0) {
    log(LOG_DEBUG,"or_handshake_client_send_auth(): bad socket while flushing.");
    return -1;
  }
  if(retval > 0) {
    /* still stuff on the buffer. */
    conn->state = OR_CONN_STATE_CLIENT_SENDING_AUTH;
    connection_watch_events(conn, POLLOUT | POLLIN);
    return 0;
  }

  /* it finished sending */
  log(LOG_DEBUG,"or_handshake_client_send_auth(): Finished sending authentication message.");
  conn->state = OR_CONN_STATE_CLIENT_AUTH_WAIT;
  connection_watch_events(conn, POLLIN);
  return 0;

}

int or_handshake_client_process_auth(connection_t *conn) {
  char buf[128]; /* only 44 of this is expected to be used */
  char cipher[128];
  uint32_t min,max;
  int retval;

  assert(conn);

  if(conn->inbuf_datalen < 128) /* entire response available? */
    return 0; /* not yet */

  if(connection_fetch_from_buf(cipher,128,conn) < 0) {
    return -1;    
  }
  log(LOG_DEBUG,"or_handshake_client_process_auth() : Received auth.");

  /* decrypt response */
  retval = RSA_private_decrypt(128,cipher,buf,conn->prkey,RSA_PKCS1_PADDING);
  if (retval == -1)
  { 
    log(LOG_ERR,"Public-key decryption failed during authentication to %s:%u.",
        conn->address,ntohs(conn->port));
    log(LOG_DEBUG,"or_handshake_client_process_auth() : Reason : %s.",
        ERR_reason_error_string(ERR_get_error()));
    return -1;
  }
  else if (retval != 44)
  { 
    log(LOG_ERR,"Received an incorrect response from router %s:%u during authentication.",
        conn->address,ntohs(conn->port));
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_client_process_auth() : Decrypted response.");
  /* check validity */
  if ( (memcmp(&conn->local.sin_addr, buf, 4)) || /* local address */
       (memcmp(&conn->local.sin_port, buf+4, 2)) || /* local port */
       (memcmp(&conn->addr, buf+6, 4)) || /* remote address */
       (memcmp(&conn->port, buf+10, 2)) || /* remote port */
       (memcmp(&conn->f_session_key, buf+12, 8)) || /* keys */
       (memcmp(&conn->b_session_key, buf+20, 8)))
  { /* incorrect response */
    log(LOG_ERR,"Router %s:%u failed to authenticate. Either the key I have is obsolete or they're doing something they're not supposed to.",conn->address,ntohs(conn->port));
    return -1;
  }

  log(LOG_DEBUG,"or_handshake_client_process_auth() : Response valid.");

  /* update link info */
  min = *(uint32_t *)(buf+28);
  max = *(uint32_t *)(buf+32);
  min = ntohl(min);
  max = ntohl(max);

  if (conn->min > min)
    conn->min = min;
  if (conn->max > max)
    conn->max = max;

  /* reply is just local addr/port, remote addr/port, nonce */
  memcpy(buf+12, buf+36, 8);

  /* encrypt reply */
  retval = RSA_public_encrypt(20,buf,cipher,conn->pkey,RSA_PKCS1_PADDING);
  if (retval == -1) /* error */
  { 
    log(LOG_ERR,"Public-key encryption failed during authentication to %s:%u.",conn->address,ntohs(conn->port));
    log(LOG_DEBUG,"or_handshake_client_process_auth() : Reason : %s.",ERR_reason_error_string(ERR_get_error()));
    return -1;
  }

  /* send the message */

  if(connection_write_to_buf(cipher, 128, conn) < 0) {
    log(LOG_DEBUG,"or_handshake_client_process_auth(): my outbuf is full. Oops.");
    return -1;
  }
  retval = connection_flush_buf(conn);
  if(retval < 0) {
    log(LOG_DEBUG,"or_handshake_client_process_auth(): bad socket while flushing.");
    return -1;
  }
  if(retval > 0) {
    /* still stuff on the buffer. */
    conn->state = OR_CONN_STATE_CLIENT_SENDING_NONCE;
    connection_watch_events(conn, POLLOUT | POLLIN);
/*    return(connection_process_inbuf(conn)); process the rest of the inbuf */
    return 0;   
  }

  /* it finished sending */
  log(LOG_DEBUG,"or_handshake_client_process_auth(): Finished sending nonce.");
  conn_or_init_crypto(conn);
  conn->state = OR_CONN_STATE_OPEN;
  connection_watch_events(conn, POLLIN);
  return connection_process_inbuf(conn); /* process the rest of the inbuf */

}

/*
 * 
 * auth handshake, as performed by OR *receiving* the connection
 *
 */

int or_handshake_server_process_auth(connection_t *conn) {
  int retval;

  char buf[128]; /* only 42 of this is expected to be used */
  char cipher[128];

  uint32_t addr;
  uint16_t port;

  uint32_t min,max;
  routerinfo_t *router;

  assert(conn);

  log(LOG_DEBUG,"or_handshake_server_process_auth() entered.");

  if(conn->inbuf_datalen < 128) /* entire response available? */
    return 0; /* not yet */  

  if(connection_fetch_from_buf(cipher,128,conn) < 0) {
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_auth() : Received auth.");

  /* decrypt response */
  retval = RSA_private_decrypt(128,cipher,buf,conn->prkey,RSA_PKCS1_PADDING);
  if (retval == -1)
  { 
    log(LOG_ERR,"Public-key decryption failed during authentication to %s:%u.",
        conn->address,ntohs(conn->port));
    log(LOG_DEBUG,"or_handshake_server_process_auth() : Reason : %s.",
        ERR_reason_error_string(ERR_get_error()));
    return -1;
  }
  else if (retval != 36)
  { 
    log(LOG_ERR,"Received an incorrect authentication request.");
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_auth() : Decrypted authentication message.");

  /* identify the router */
  memcpy(&addr,buf,4); /* save the IP address */
  memcpy(&port,buf+4,2); /* save the port */

  router = router_get_by_addr_port(addr,port);
  if (!router)
  {
    log(LOG_DEBUG,"or_handshake_server_process_auth() : Received a connection from an unknown router. Will drop.");
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_auth() : Router identified as %s:%u.",
      router->address,ntohs(router->port));

  if(connection_get_by_addr_port(addr,port)) {
    log(LOG_DEBUG,"or_handshake_server_process_auth(): That router is already connected. Dropping.");
    return -1;
  }

  /* save keys */
  memcpy(conn->b_session_key,buf+12,8);
  memcpy(conn->f_session_key,buf+20,8);

  /* update link info */
  min = *(uint32_t *)(buf+28);
  max = *(uint32_t *)(buf+32);
  min = ntohl(min);
  max = ntohl(max);

  conn->min = router->min;
  conn->max = router->max;

  if (conn->min > min)
    conn->min = min;
  if (conn->max > max)
    conn->max = max;

  /* copy all relevant info to conn */
  conn->addr = router->addr, conn->port = router->port;
  conn->pkey = router->pkey;
  conn->address = strdup(router->address);

  /* generate a nonce */
  retval = RAND_pseudo_bytes(conn->nonce,8);
  if (retval == -1) /* error */
  {
    log(LOG_ERR,"Cannot generate a nonce.");
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_auth() : Nonce generated.");

  /* generate message */
  memcpy(buf+36,conn->nonce,8); /* append the nonce to the end of the message */
  *(uint32_t *)(buf+28) = htonl(conn->min); /* send min link utilisation */
  *(uint32_t *)(buf+32) = htonl(conn->max); /* send max link utilisation */

  /* encrypt message */
  retval = RSA_public_encrypt(44,buf,cipher,conn->pkey,RSA_PKCS1_PADDING);
  if (retval == -1) /* error */
  {
    log(LOG_ERR,"Public-key encryption failed during authentication to %s:%u.",conn->address,ntohs(conn->port));
    log(LOG_DEBUG,"or_handshake_server_process_auth() : Reason : %s.",ERR_reason_error_string(ERR_get_error()));
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_auth() : Reply encrypted.");

  /* send message */

  if(connection_write_to_buf(cipher, 128, conn) < 0) {
    log(LOG_DEBUG,"or_handshake_server_process_auth(): my outbuf is full. Oops.");
    return -1;
  }
  retval = connection_flush_buf(conn);
  if(retval < 0) {
    log(LOG_DEBUG,"or_handshake_server_process_auth(): bad socket while flushing.");
    return -1;
  }
  if(retval > 0) {
    /* still stuff on the buffer. */
    conn->state = OR_CONN_STATE_SERVER_SENDING_AUTH;
    connection_watch_events(conn, POLLOUT | POLLIN);
    return 0;
  }

  /* it finished sending */
  log(LOG_DEBUG,"or_handshake_server_process_auth(): Finished sending auth.");
  conn->state = OR_CONN_STATE_SERVER_NONCE_WAIT;
  connection_watch_events(conn, POLLIN);
  return 0;

}

int or_handshake_server_process_nonce(connection_t *conn) {

  char buf[128];
  char cipher[128];
  int retval;

  assert(conn);

  if(conn->inbuf_datalen < 128) /* entire response available? */
    return 0; /* not yet */

  if(connection_fetch_from_buf(cipher,128,conn) < 0) {
    return -1;    
  }
  log(LOG_DEBUG,"or_handshake_server_process_nonce() : Received auth.");

  /* decrypt response */
  retval = RSA_private_decrypt(128,cipher,buf,conn->prkey,RSA_PKCS1_PADDING);
  if (retval == -1)
  {
    log(LOG_ERR,"Public-key decryption failed during authentication to %s:%u.",
        conn->address,ntohs(conn->port));
    log(LOG_DEBUG,"or_handshake_server_process_nonce() : Reason : %s.",
        ERR_reason_error_string(ERR_get_error()));
    return -1;
  }
  else if (retval != 20)
  { 
    log(LOG_ERR,"Received an incorrect response from router %s:%u during authentication.",
        conn->address,ntohs(conn->port));
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_nonce() : Response decrypted.");

  /* check validity */
  if ((memcmp(&conn->addr,buf, 4)) || /* remote address */
      (memcmp(&conn->port,buf+4,2)) || /* remote port */
      (memcmp(&conn->local.sin_addr,buf+6,4)) || /* local address */
      (memcmp(&conn->local.sin_port,buf+10,2)) || /* local port */
      (memcmp(conn->nonce,buf+12,8))) /* nonce */
  { 
    log(LOG_ERR,"Router %s:%u failed to authenticate. Either the key I have is obsolete or they're doing something they're not supposed to.",conn->address,ntohs(conn->port));
    return -1;
  }
  log(LOG_DEBUG,"or_handshake_server_process_nonce() : Response valid. Authentication complete.");

  conn_or_init_crypto(conn);
  conn->state = OR_CONN_STATE_OPEN;
  connection_watch_events(conn, POLLIN);
  return connection_process_inbuf(conn); /* process the rest of the inbuf */

}


/* ********************************** */


int connection_or_create_listener(RSA *prkey, struct sockaddr_in *local) {
  log(LOG_DEBUG,"connection_create_or_listener starting");
  return connection_create_listener(prkey, local, CONN_TYPE_OR_LISTENER);
}

int connection_or_handle_listener_read(connection_t *conn) {
  log(LOG_NOTICE,"OR: Received a connection request from a router. Attempting to authenticate.");
  return connection_handle_listener_read(conn, CONN_TYPE_OR, OR_CONN_STATE_SERVER_AUTH_WAIT);
}

