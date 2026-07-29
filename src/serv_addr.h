#ifndef SERV_ADDR_H
#define SERV_ADDR_H

#ifdef __cplusplus
extern "C" {
#endif

int serv_addr_init(const char *initial_addr);
int serv_addr_try_promote(const char *candidate_addr);
const char *get_serv_addr(void);

#ifdef __cplusplus
}
#endif

#endif /* SERV_ADDR_H */
