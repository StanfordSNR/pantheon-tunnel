/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <thread>
#include <chrono>

#include <sys/socket.h>
#include <net/route.h>

#include "tunnelserver.hh"
#include "netdevice.hh"
#include "system_runner.hh"
#include "util.hh"
#include "interfaces.hh"
#include "address.hh"
#include "timestamp.hh"
#include "exception.hh"
#include "bindworkaround.hh"
#include "config.h"

using namespace std;
using namespace PollerShortNames;

TunnelServer::TunnelServer( char ** const user_environment,
                                            const std::string & ingress_logfile,
                                            const std::string & egress_logfile )
    : user_environment_( user_environment ),
      egress_ingress( two_unassigned_addresses( get_mahimahi_base() ) ),
      listening_socket_(),
      event_loop_(),
      ingress_log_(),
      egress_log_()
{
    /* make sure environment has been cleared */
    if ( environ != nullptr ) {
        throw runtime_error( "TunnelServer: environment was not cleared" );
    }

    /* initialize base timestamp value before any forking */
    initial_timestamp();

    /* open logfiles if called for */
    if ( not ingress_logfile.empty() ) {
        ingress_log_.reset( new ofstream( ingress_logfile ) );
        if ( not ingress_log_->good() ) {
            throw runtime_error( ingress_logfile + ": error opening for writing" );
        }

        *ingress_log_ << "# mahimahi mm-tunnelserver ingress: " << initial_timestamp() << endl;
    }
    if ( not egress_logfile.empty() ) {
        egress_log_.reset( new ofstream( egress_logfile ) );
        if ( not egress_log_->good() ) {
            throw runtime_error( egress_logfile + ": error opening for writing" );
        }

        *egress_log_ << "# mahimahi mm-tunnelserver egress: " << initial_timestamp() << endl;
    }

    /* bind the listening socket to an available address/port, and print out what was bound */
    listening_socket_.bind( Address() );
    cout << "mm-tunnelclient localhost " << listening_socket_.local_address().port() << " " << egress_addr().ip() << " " << ingress_addr().ip() << endl;

    std::pair<Address, std::string> recpair =  listening_socket_.recvfrom( );
    cout << "got connection from " << recpair.first.ip() << endl;
    listening_socket_.connect( recpair.first );
}

void TunnelServer::start_link( const string & shell_prefix,
                                                const vector< string > & command)
{
    /* Fork */
    event_loop_.add_child_process( "packetshell", [&]() { // XXX add special child process?
            TunDevice ingress_tun( "ingress", ingress_addr(), egress_addr() );

            /* bring up localhost */
            interface_ioctl( SIOCSIFFLAGS, "lo",
                             [] ( ifreq &ifr ) { ifr.ifr_flags = IFF_UP; } );

            /* create default route */
            rtentry route;
            zero( route );

            route.rt_gateway = egress_addr().to_sockaddr();
            route.rt_dst = route.rt_genmask = Address().to_sockaddr();
            route.rt_flags = RTF_UP | RTF_GATEWAY;

            SystemCall( "ioctl SIOCADDRT", ioctl( UDPSocket().fd_num(), SIOCADDRT, &route ) );

            EventLoop inner_loop;

            /* Fork again after dropping root privileges */
            drop_privileges();

            /* restore environment */
            environ = user_environment_;

            /* set MAHIMAHI_BASE if not set already to indicate outermost container */
            SystemCall( "setenv", setenv( "MAHIMAHI_BASE",
                                          egress_addr().ip().c_str(),
                                          false /* don't override */ ) );

            inner_loop.add_child_process( join( command ), [&]() {
                    /* tweak bash prompt */
                    prepend_shell_prefix( shell_prefix );

                    return ezexec( command, true );
                } );


            /* ingress_tun device gets datagram -> read it -> give to server socket */
            inner_loop.add_simple_input_handler( ingress_tun,
                    [&] () {
                    const string packet = ingress_tun.read();

                    const uint64_t uid_to_send = uid_++;

                    if ( egress_log_ ) {
                    *egress_log_ << timestamp() << " - " << uid_to_send << " - " << packet.length() << endl;
                    }

                    listening_socket_.write( string( (char *) &uid_to_send, sizeof(uid_to_send) ) + packet );

                    return ResultType::Continue;
                    } );

            /* we get datagram from listening_socket_ process -> write it to ingress_tun device */
            inner_loop.add_simple_input_handler( listening_socket_,
                    [&] () {
                    const string packet = listening_socket_.read();

                    uint64_t uid_received = *( (uint64_t *) packet.data() );
                    string contents = packet.substr( sizeof(uid_received) );

                    if ( ingress_log_ ) {
                    *ingress_log_ << timestamp() << " - " << uid_received << " - " << packet.length() << endl;
                    }

                    ingress_tun.write( contents );
                    return ResultType::Continue;
                    } );

            /* exit if finished
            inner_loop.add_action( Poller::Action( listening_socket_, Direction::Out,
                        [&] () {
                        return ResultType::Exit;
                        } ); */

            return inner_loop.loop();
        }, true );  /* new network namespace */
}

int TunnelServer::wait_for_exit( void )
{
    return event_loop_.loop();
}

struct TemporaryEnvironment
{
    TemporaryEnvironment( char ** const env )
    {
        if ( environ != nullptr ) {
            throw runtime_error( "TemporaryEnvironment: cannot be entered recursively" );
        }
        environ = env;
    }

    ~TemporaryEnvironment()
    {
        environ = nullptr;
    }
};

Address TunnelServer::get_mahimahi_base( void ) const
{
    /* temporarily break our security rule of not looking
       at the user's environment before dropping privileges */
    TemporarilyUnprivileged tu;
    TemporaryEnvironment te { user_environment_ };

    const char * const mahimahi_base = getenv( "MAHIMAHI_BASE" );
    if ( not mahimahi_base ) {
        return Address();
    }

    return Address( mahimahi_base, 0 );
}
