/*
  dst_addr.hpp
  
  $Id: dst_addr.hpp,v 1.2 2002/05/06 03:02:40 evertonm Exp $
 */

#ifndef DST_ADDR_HPP
#define DST_ADDR_HPP

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "to_addr.hpp"

class dst_addr : public to_addr
{
private:
  char           *name;
  struct ip_addr address;
  int            port;

public:
  dst_addr(char *hostname, struct ip_addr addr, int prt) 
    {
      name    = hostname;
      address = addr;
      port    = prt;
    }

  void show() const;
  int get_addr(const char *protoname, const struct ip_addr **addr, int *prt);
};

#endif /* DST_ADDR_HPP */

/* Eof: dst_addr.hpp */
