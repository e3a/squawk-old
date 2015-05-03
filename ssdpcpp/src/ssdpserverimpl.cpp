/*
    SSDP Server

    Copyright (C) 2015  <etienne> <e.knecht@netwings.ch>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <ctime>
#include <future>
#include <iostream>
#include <sstream>
#include <string>

#include "ssdp.h"
#include "ssdpserverconnection.h"
#include "ssdpclientconnection.h"

namespace ssdp {

SSDPServerImpl::SSDPServerImpl ( const std::string & uuid, const std::string & multicast_address, const int & multicast_port ) :
	uuid ( uuid ), multicast_address ( multicast_address ), multicast_port ( multicast_port ) {
}

void SSDPServerImpl::start() {
	//start the server
	connection = std::unique_ptr<SSDPServerConnection> (
					 new ssdp::SSDPServerConnection ( multicast_address, multicast_port ) );
	connection->set_handler ( this );
	connection->start();

	//start reannounce thread
	announce_thread_run = true;
	annouceThreadRunner = std::unique_ptr<std::thread> (
							  new std::thread ( &SSDPServerImpl::annouceThread, this ) );
}
/**
 * Stop the server.
 */
void SSDPServerImpl::stop() {
	//stop reannounce thread
	suppress();
	announce_thread_run = false;
	annouceThreadRunner->join();

	//stop the server
	connection->stop();
}
void SSDPServerImpl::handle_response ( ::http::HttpResponse & response ) {
    if ( response.status() == http::http_status::OK ) {
		if ( response.parameter ( UPNP_HEADER_USN ).find ( uuid ) == string::npos ) {
			upnp_devices[response.parameter ( UPNP_HEADER_USN )] = parseResponse ( response );
			fireEvent ( SSDPEventListener::ANNOUNCE, response.remote_ip, parseResponse ( response ) );
		}
	}
}
void SSDPServerImpl::handle_receive ( ::http::HttpRequest & request ) {
    if ( request.method() == REQUEST_METHOD_MSEARCH ) {
        if ( request.parameter( UPNP_HEADER_ST ) == NS_ROOT_DEVICE || request.parameter( UPNP_HEADER_ST ) == UPNP_NS_ALL ) {
			for ( auto & iter : namespaces ) {
				Response response ( Response::ok, HTTP_REQUEST_LINE_OK, create_response ( iter.first, iter.second ) );
				connection->send ( response );
			}

        } else if ( namespaces.find ( request.parameter( UPNP_HEADER_ST ) ) != namespaces.end() ) {
			connection->send ( Response ( Response::ok, HTTP_REQUEST_LINE_OK,
                                          create_response ( request.parameter( UPNP_HEADER_ST ),
                                                  namespaces[request.parameter( UPNP_HEADER_ST ) ] ) ) );
		}

    } else if ( request.method() == REQUEST_METHOD_NOTIFY ) {

        if ( request.parameter( UPNP_HEADER_NTS ) == UPNP_STATUS_ALIVE ) {
			// do not process own messages received over other interface
            if ( request.parameter( UPNP_HEADER_USN  ).find ( uuid ) == string::npos ) {
                upnp_devices[ request.parameter( UPNP_HEADER_USN ) ] = parseRequest ( request );
                fireEvent ( SSDPEventListener::ANNOUNCE, request.remoteIp(), parseRequest ( request ) );
			}

		} else {
			// do not process own messages received over other interface
            if ( request.parameter( UPNP_HEADER_USN ).find ( uuid ) == string::npos ) {
                fireEvent ( SSDPEventListener::BYE, request.remoteIp(), parseRequest ( request ) );
                upnp_devices.erase ( request.parameter( UPNP_HEADER_USN ) );
			}
		}

	} else {
		std::cerr << "other response: " << request << std::endl;
	}
}
UpnpDevice SSDPServerImpl::parseRequest ( http::HttpRequest & request ) {
	time_t cache_control = 0;

    if ( request.containsParameter( http::header::CACHE_CONTROL ) )
        cache_control = ( commons::string::starts_with ( request.parameter( http::header::CACHE_CONTROL ), UPNP_OPTION_MAX_AGE ) ?
                          commons::string::parse_string<time_t> ( request.parameter( http::header::CACHE_CONTROL ).substr ( UPNP_OPTION_MAX_AGE.size() ) ) : 0 );

    return UpnpDevice ( request.parameter( http::header::HOST ), request.parameter( UPNP_HEADER_LOCATION ),
                        request.parameter( UPNP_HEADER_NT ), request.parameter( UPNP_HEADER_NTS ),
                        request.parameter( UPNP_HEADER_SERVER ), request.parameter( UPNP_HEADER_USN ),
						std::time ( 0 ), cache_control );
}
UpnpDevice SSDPServerImpl::parseResponse ( http::HttpResponse & response ) {
	time_t cache_control = 0;

    if ( response.containsParameter ( http::header::CACHE_CONTROL ) )
        cache_control = ( commons::string::starts_with ( response.parameter ( http::header::CACHE_CONTROL ), UPNP_OPTION_MAX_AGE ) ?
                          commons::string::parse_string<time_t> ( response.parameter ( http::header::CACHE_CONTROL ).substr ( UPNP_OPTION_MAX_AGE.size() ) ) : 0 );

    return UpnpDevice ( response.parameter ( http::header::HOST ), response.parameter ( UPNP_HEADER_LOCATION ),
						response.parameter ( UPNP_HEADER_NT ), response.parameter ( UPNP_HEADER_NTS ),
						response.parameter ( UPNP_HEADER_SERVER ), response.parameter ( UPNP_HEADER_USN ),
						std::time ( 0 ), cache_control );
}
void SSDPServerImpl::announce() {
	suppress();

	for ( size_t i = 0; i < NETWORK_COUNT; i++ ) {
		for ( auto & iter : namespaces ) {
			send_anounce ( iter.first, iter.second );
		}
	}
}
void SSDPServerImpl::suppress() {
	for ( size_t i = 0; i < NETWORK_COUNT; i++ ) {
		for ( auto & iter : namespaces ) {
			send_suppress ( iter.first );
		}
	}
}
void SSDPServerImpl::search ( const std::string & service ) {

	std::async ( std::launch::async, [this, &service]() {

		std::map< std::string, std::string > map;
        map[http::header::HOST] = multicast_address + std::string ( ":" ) + commons::string::to_string<int> ( multicast_port );
		map[commons::string::to_upper ( UPNP_HEADER_ST )] = service;
		map[commons::string::to_upper ( UPNP_HEADER_MX )] = "2";
		map[commons::string::to_upper ( UPNP_HEADER_MAN )] = UPNP_STATUS_DISCOVER;
        map[http::header::CONTENT_LENGTH] = std::string ( "0" );

		SSDPClientConnection connection ( this, multicast_address, multicast_port );
		connection.send ( SSDP_HEADER_SEARCH_REQUEST_LINE, map );
		std::this_thread::sleep_for ( std::chrono::seconds ( 20 ) );
	} );
}
std::map< std::string, std::string > SSDPServerImpl::create_response ( const std::string & nt, const std::string & location ) {

	std::map< std::string, std::string > map;
    map[http::header::CACHE_CONTROL] = UPNP_OPTION_MAX_AGE + commons::string::to_string<int> ( ANNOUNCE_INTERVAL );
	map[UPNP_HEADER_LOCATION] = location;
	map[UPNP_HEADER_SERVER] = commons::system::uname() + std::string ( " DLNADOC/1.50 UPnP/1.0 SSDP/1.0.0" ); //TODO
	map[commons::string::to_upper ( UPNP_HEADER_ST )] = nt;
	map[commons::string::to_upper ( UPNP_HEADER_USN )] = std::string ( "uuid:" ) + uuid + std::string ( "::" ) + nt;
	map[commons::string::to_upper ( UPNP_HEADER_EXT )] = "";
    map[http::header::DATE] = commons::system::time_string();
    map[http::header::CONTENT_LENGTH] = std::string ( "0" );

	return map;
}
void SSDPServerImpl::send_anounce ( const std::string & nt, const std::string & location ) {

	std::map< std::string, std::string > map;
    map[http::header::HOST] = multicast_address + std::string ( ":" ) + commons::string::to_string<int> ( multicast_port );
    map[http::header::CACHE_CONTROL] = UPNP_OPTION_MAX_AGE + commons::string::to_string<int> ( ANNOUNCE_INTERVAL );
	map[commons::string::to_upper ( UPNP_HEADER_LOCATION )] = location;
	map[commons::string::to_upper ( UPNP_HEADER_SERVER )] = commons::system::uname() + " " + USER_AGENT;
	map[commons::string::to_upper ( UPNP_HEADER_NT )] = nt;
	map[commons::string::to_upper ( UPNP_HEADER_USN )] = "uuid:" + uuid + "::" + nt;
	map[commons::string::to_upper ( UPNP_HEADER_NTS )] = UPNP_STATUS_ALIVE;
	map[commons::string::to_upper ( UPNP_HEADER_EXT )] = std::string ( "" );
	map[commons::string::to_upper ( UPNP_HEADER_DATE )] = commons::system::time_string();
    map[http::header::CONTENT_LENGTH] = std::string ( "0" );

	connection->send ( SSDP_HEADER_REQUEST_LINE, map );
}
void SSDPServerImpl::send_suppress ( const std::string & nt ) {

	std::map< std::string, std::string > map;
    map[http::header::HOST] = multicast_address + std::string ( ":" ) + commons::string::to_string<int> ( multicast_port );
	map[commons::string::to_upper ( UPNP_HEADER_NT )] = nt;
	map[commons::string::to_upper ( UPNP_HEADER_USN )] = "uuid:" + uuid + "::" + nt;
	map[commons::string::to_upper ( UPNP_HEADER_NTS )] = UPNP_STATUS_BYE;
	map[commons::string::to_upper ( UPNP_HEADER_SERVER )] = commons::system::uname() + " " + USER_AGENT;
	map[commons::string::to_upper ( UPNP_HEADER_EXT )] = std::string ( "" );
	map[commons::string::to_upper ( UPNP_HEADER_DATE )] = commons::system::time_string();
    map[http::header::CONTENT_LENGTH] = std::string ( "0" );

	connection->send ( SSDP_HEADER_REQUEST_LINE, map );
}
void SSDPServerImpl::annouceThread() {
	start_time = std::chrono::high_resolution_clock::now();

	while ( announce_thread_run ) {
		auto end_time = std::chrono::high_resolution_clock::now();
		auto dur = end_time - start_time;
		auto f_secs = std::chrono::duration_cast<std::chrono::duration<unsigned int>> ( dur );

		if ( f_secs.count() >= ( ANNOUNCE_INTERVAL / 3 ) ) {
			for ( size_t i = 0; i < NETWORK_COUNT; i++ ) {
				for ( auto & iter : namespaces ) {
					send_anounce ( iter.first, iter.second );
				}
			}

			for ( auto device : upnp_devices ) {
				if ( device.second.last_seen + device.second.cache_control >= std::time ( 0 ) ) {
					std::cerr << "remove device: " << device.first << std::endl;
					// upnp_devices.erase( device.first ); //TODO use lock
				}
			}

			start_time = std::chrono::high_resolution_clock::now();
		}

		std::this_thread::sleep_for ( std::chrono::milliseconds ( 5000 ) );
	}
}
void SSDPServerImpl::fireEvent ( SSDPEventListener::EVENT_TYPE type, std::string  client_ip, UpnpDevice device ) const {
	for ( auto & listener : listeners ) {
		listener->ssdpEvent ( type, client_ip, device );
	}
}
}
