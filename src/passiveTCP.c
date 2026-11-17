int passivesock(const char *service, const char *transport, int backlog);
int passiveTCP(const char *service, int backlog) { return passivesock(service, "tcp", backlog); }
