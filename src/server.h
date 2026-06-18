#ifndef SERVER_H
#define SERVER_H

/* Creates, binds and starts listening on a TCP socket for the given port
 * (any local address). Sets SO_REUSEADDR so restarts don't hit
 * "Address already in use", and SO_REUSEPORT so multiple worker processes
 * can each bind their own socket to the same port. Returns the listening
 * fd, or -1 on error (errno set, a diagnostic has already been printed to
 * stderr). */
int server_listen(int port, int backlog);

#endif /* SERVER_H */
