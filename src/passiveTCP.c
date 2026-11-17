// Envoltorio para abrir un socket TCP en modo pasivo
int passivesock(const char *service, const char *transport, int backlog);
int passiveTCP(const char *service, int backlog) { return passivesock(service, "tcp", backlog); }
