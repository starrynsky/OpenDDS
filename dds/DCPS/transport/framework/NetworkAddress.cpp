/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "DCPS/DdsDcps_pch.h" //Only the _pch include should start with DCPS/
#include "NetworkAddress.h"
#include "ace/OS_NS_netdb.h"
#include "ace/Sock_Connect.h"
#include "ace/OS_NS_sys_socket.h" // For setsockopt()


#if !defined (__ACE_INLINE__)
# include "NetworkAddress.inl"
#endif /* !__ACE_INLINE__ */

ACE_BEGIN_VERSIONED_NAMESPACE_DECL

ACE_CDR::Boolean
operator<< (ACE_OutputCDR& outCdr, OpenDDS::DCPS::NetworkAddress& value)
{
  outCdr << ACE_OutputCDR::from_boolean(ACE_CDR_BYTE_ORDER);

  outCdr << ACE_OutputCDR::from_octet(value.reserved_);
  outCdr << value.addr_.c_str();

  return outCdr.good_bit();
}

ACE_CDR::Boolean
operator>> (ACE_InputCDR& inCdr, OpenDDS::DCPS::NetworkAddress& value)
{
  CORBA::Boolean byte_order;

  if ((inCdr >> ACE_InputCDR::to_boolean(byte_order)) == 0)
    return 0;

  inCdr.reset_byte_order(byte_order);

  if ((inCdr >> ACE_InputCDR::to_octet(value.reserved_)) == 0)
    return 0;

  char* buf = 0;

  if ((inCdr >> buf) == 0)
    return 0;

  value.addr_ = buf;

  delete[] buf;

  return inCdr.good_bit();
}

ACE_END_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace DCPS {

OPENDDS_STRING get_fully_qualified_hostname(ACE_INET_Addr* addr)
{
  // cache the determined fully qualified hostname and its
  // address to be used on subsequent calls
  static OPENDDS_STRING fullname;
  static ACE_INET_Addr selected_address;

  if (fullname.length() == 0) {
    size_t addr_count;
    ACE_INET_Addr *addr_array;
    OpenDDS::DCPS::HostnameInfoVector nonFQDN;

    int result = ACE::get_ip_interfaces(addr_count, addr_array);

    struct Array_Guard {
      Array_Guard(ACE_INET_Addr *ptr) : ptr_(ptr) {}
      ~Array_Guard() {
        delete [] ptr_;
      }
      ACE_INET_Addr* const ptr_;
    } guardObject(addr_array);

    if (result != 0 || addr_count < 1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) ERROR: Unable to probe network. %p\n"),
                 ACE_TEXT("ACE::get_ip_interfaces")));

    } else {
#ifdef ACE_HAS_IPV6
        //front load IPV6 addresses to give preference to IPV6 interfaces
        size_t index_last_non_ipv6 = 0;
        for (size_t i = 0; i < addr_count; i++) {
          if (addr_array[i].get_type() == AF_INET6) {
            if (i == index_last_non_ipv6) {
              ++index_last_non_ipv6;
            } else {
              std::swap(addr_array[i], addr_array[index_last_non_ipv6]);
              ++index_last_non_ipv6;
            }
          }
        }
#endif
      for (size_t i = 0; i < addr_count; i++) {
        char hostname[MAXHOSTNAMELEN+1] = "";

        //Discover the fully qualified hostname

        if (ACE::get_fqdn(addr_array[i], hostname, MAXHOSTNAMELEN+1) == 0) {
          if (addr_array[i].is_loopback() == false && ACE_OS::strchr(hostname, '.') != 0) {
            VDBG_LVL((LM_DEBUG, "(%P|%t) found fqdn %C from %C:%d\n",
                      hostname, addr_array[i].get_host_addr(), addr_array[i].get_port_number()), 2);
            selected_address = addr_array[i];
            fullname = hostname;
            if (addr) {
              *addr = selected_address;
            }
            return fullname;

          } else {
            VDBG_LVL((LM_DEBUG, "(%P|%t) ip interface %C:%d maps to hostname %C\n",
                      addr_array[i].get_host_addr(), addr_array[i].get_port_number(), hostname), 2);

            if (ACE_OS::strncmp(hostname, "localhost", 9) == 0) {
              addr_array[i].get_host_addr(hostname, MAXHOSTNAMELEN);
            }

            OpenDDS::DCPS::HostnameInfo info;
            info.index_ = i;
            info.hostname_ = hostname;
            nonFQDN.push_back(info);
          }
        }
      }
    }

    OpenDDS::DCPS::HostnameInfoVector::iterator itBegin = nonFQDN.begin();
    OpenDDS::DCPS::HostnameInfoVector::iterator itEnd = nonFQDN.end();

    for (OpenDDS::DCPS::HostnameInfoVector::iterator it = itBegin; it != itEnd; ++it) {
      if (addr_array[it->index_].is_loopback() == false) {
        ACE_DEBUG((LM_WARNING, "(%P|%t) WARNING: Could not find FQDN. Using "
                   "\"%C\" as fully qualified hostname, please "
                   "correct system configuration.\n", it->hostname_.c_str()));
        selected_address = addr_array[it->index_];
        fullname = it->hostname_;
        if (addr) {
          *addr = selected_address;
        }
        return fullname;
      }
    }

    if (itBegin != itEnd) {
      ACE_DEBUG((LM_WARNING, "(%P|%t) WARNING: Could not find FQDN. Using "
                 "\"%C\" as fully qualified hostname, please "
                 "correct system configuration.\n", itBegin->hostname_.c_str()));
      selected_address = addr_array[itBegin->index_];
      fullname = itBegin->hostname_;
      if (addr) {
        *addr = selected_address;
      }
      return fullname;
    }

#ifdef OPENDDS_SAFETY_PROFILE
    // address resolution may not be available due to safety profile,
    // return an address that should work for running tests
    if (addr) {
      static const char local[] = {1, 0, 0, 127};
      addr->set_address(local, sizeof local);
    }
    return "localhost";
#else
    ACE_ERROR((LM_ERROR,
               "(%P|%t) ERROR: failed to discover the fully qualified hostname\n"));
#endif
  }

  if (addr) {
    *addr = selected_address;
  }
  return fullname;
}

bool set_socket_multicast_ttl(const ACE_SOCK_Dgram& socket, const unsigned char& ttl)
{
  ACE_HANDLE handle = socket.get_handle();
#if defined (ACE_HAS_IPV6)
  ACE_INET_Addr local_addr;
  if (0 != socket.get_local_addr(local_addr)) {
  VDBG((LM_WARNING, "(%P|%t) set_socket_ttl: "
      "ACE_SOCK_Dgram::get_local_addr %p\n", ACE_TEXT("")));
  }
  if (local_addr.get_type () == AF_INET6) {
    if (0 != ACE_OS::setsockopt(handle,
                                IPPROTO_IPV6,
                                IPV6_MULTICAST_HOPS,
                                reinterpret_cast<const char*>(&ttl),
                                sizeof(ttl))) {
      ACE_ERROR_RETURN((LM_ERROR,
                        ACE_TEXT("(%P|%t) ERROR: ")
                        ACE_TEXT("set_socket_ttl: ")
                        ACE_TEXT("failed to set IPV6 TTL: %C %p\n"),
                        ttl,
                        ACE_TEXT("ACE_OS::setsockopt(TTL)")),
                       false);
    }
  } else
#endif  /* ACE_HAS_IPV6 */
  if (0 != ACE_OS::setsockopt(handle,
                              IPPROTO_IP,
                              IP_MULTICAST_TTL,
                              reinterpret_cast<const char*>(&ttl),
                              sizeof(ttl))) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("set_socket_ttl: ")
                      ACE_TEXT("failed to set TTL: %C %p\n"),
                      ttl,
                      ACE_TEXT("ACE_OS::setsockopt(TTL)")),
                     false);
  }
  return true;
}

bool open_dual_stack_socket(ACE_SOCK_Dgram& socket, bool active, const ACE_INET_Addr& local_address)
{
#if defined (ACE_HAS_IPV6) && defined (IPV6_V6ONLY)
  int protocol_family = ACE_PROTOCOL_FAMILY_INET;
  int protocol = 0;
  int reuse_addr = 0;
  if ((ACE_Addr)local_address != ACE_Addr::sap_any)
    protocol_family = local_address.get_type();
  else if (protocol_family == PF_UNSPEC)
    protocol_family = ACE::ipv6_enabled() ? PF_INET6 : PF_INET;

  int one = 1;
  socket.set_handle(ACE_OS::socket(protocol_family,
    SOCK_DGRAM,
    protocol));

  if (socket.get_handle() == ACE_INVALID_HANDLE) {
    ACE_ERROR_RETURN((LM_ERROR,
      ACE_TEXT("(%P|%t) ERROR: ")
      ACE_TEXT("open_dual_stack_socket: ")
      ACE_TEXT("failed to set socket handle\n")),
      false);
  }
  else if (protocol_family != PF_UNIX
    && reuse_addr
    && socket.set_option(SOL_SOCKET,
      SO_REUSEADDR,
      &one,
      sizeof one) == -1)
  {
    socket.close();
    ACE_ERROR_RETURN((LM_ERROR,
      ACE_TEXT("(%P|%t) ERROR: ")
      ACE_TEXT("open_dual_stack_socket: ")
      ACE_TEXT("failed to set socket SO_REUSEADDR option\n")),
      false);
  }
  if (!active && local_address.get_type() == AF_INET6) {
    ACE_HANDLE handle = socket.get_handle();
    int ipv6_only = 0;
    if (0 != ACE_OS::setsockopt(handle,
      IPPROTO_IPV6,
      IPV6_V6ONLY,
      (char*)&ipv6_only,
      sizeof(ipv6_only))) {
      ACE_ERROR_RETURN((LM_ERROR,
        ACE_TEXT("(%P|%t) ERROR: ")
        ACE_TEXT("open_dual_stack_socket: ")
        ACE_TEXT("failed to set IPV6_V6ONLY to 0: %p\n"),
        ACE_TEXT("ACE_OS::setsockopt(IPV6_V6ONLY)")),
        false);
    }
  }
  bool error = false;

  if ((ACE_Addr)local_address == ACE_Addr::sap_any)
  {
    if (protocol_family == PF_INET || protocol_family == PF_INET6)
    {
      if (ACE::bind_port(socket.get_handle(),
        INADDR_ANY,
        protocol_family) == -1)
        error = true;
    }
  }
  else if (ACE_OS::bind(socket.get_handle(),
    reinterpret_cast<sockaddr *> (local_address.get_addr()),
    local_address.get_size()) == -1)
    error = true;

  if (error) {
    socket.close();
    ACE_ERROR_RETURN((LM_ERROR,
      ACE_TEXT("(%P|%t) ERROR: ")
      ACE_TEXT("open_dual_stack_socket: ")
      ACE_TEXT("failed to bind address to socket\n")),
      false);
  }
  return true;
#endif
  return false;
}
}
}
