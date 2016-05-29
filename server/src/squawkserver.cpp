/*
    <one line to give the library's name and an idea of what it does.>
    Copyright (C) 2013  <copyright holder> <email>

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

#include "squawkserver.h"


#include <string>
#include <iostream>
#include <thread>
#include <exception>
#include <signal.h>


#include "fileservlet.h"

#include "ssdp.h"

#include "upnpcontentdirectory.h"
#include "upnpcontentdirectoryapi.h"
#include "upnpcontentdirectorydao.h"
#include "upnpcontentdirectoryfile.h"
#include "upnpcontentdirectoryimage.h"
#include "upnpcontentdirectorymusic.h"
#include "upnpcontentdirectoryparser.h"
#include "upnpcontentdirectoryvideo.h"
#include "upnpmediaservlet.h"
#include "upnpxmldescription.h"
#include "upnpcontentdirectorydao.h"
#include "upnpcontentdirectoryparser.h"
#include "upnpconnectionmanager.h"

namespace squawk {

log4cxx::LoggerPtr SquawkServer::logger ( log4cxx::Logger::getLogger ( "squawk.SquawkServer" ) );

void SquawkServer::start( squawk::SquawkConfig * squawk_config ) {

    _squawk_config = std::shared_ptr< squawk::SquawkConfig >(squawk_config);

    // create the server
    _upnp_cds_dao = std::shared_ptr< squawk::UpnpContentDirectoryDao >( new squawk::UpnpContentDirectoryDao() );
    _upnp_file_parser = std::shared_ptr< squawk::UpnpContentDirectoryParser >( new squawk::UpnpContentDirectoryParser() );

    //Setup and start the SSDP server
    ssdp_server = new ssdp::SSDPServerImpl(
    squawk_config->uuid(),
    squawk_config->multicastAddress(),
    squawk_config->multicastPort() );

    //register namespaces
    ssdp_server->register_namespace(squawk_config->uuid(), std::string("http://") + squawk_config->httpAddress() + ":" + std::to_string( squawk_config->httpPort() ) + "/rootDesc.xml");
    ssdp_server->register_namespace(ssdp::NS_ROOT_DEVICE, std::string("http://") + squawk_config->httpAddress() + ":" + std::to_string( squawk_config->httpPort() ) + "/rootDesc.xml");
    ssdp_server->register_namespace(ssdp::NS_MEDIASERVER, std::string("http://") + squawk_config->httpAddress() + ":" + std::to_string( squawk_config->httpPort() ) + "/rootDesc.xml");
    ssdp_server->register_namespace(ssdp::NS_CONTENT_DIRECTORY, std::string("http://") + squawk_config->httpAddress() + ":" + std::to_string( squawk_config->httpPort() ) + "/rootDesc.xml");
//    ssdp_server->register_namespace(ssdp::NS_MEDIA_RECEIVER_REGISTRAR, std::string("http://") + squawk_config->httpAddress() + ":" + commons::string::to_string( squawk_config->httpPort() ) + "/rootDesc.xml");
    ssdp_server->subscribe(
        [this](ssdp::SSDPEventListener::EVENT_TYPE type, std::string, ssdp::SsdpEvent device ) {

 //TODO           std::lock_guard<std::mutex> _ssdp_devices_guard( _ssdp_devices_mutex );

            //nt            device type
            //nts           event type
            //usn           device endpoint uuid
            //cache_control cache timer
            std::string _rootdevice_usn;
            size_t uuid_position_end_ = device.usn().find( "::" );
            if( uuid_position_end_ != std::string::npos ) {
                _rootdevice_usn = device.usn().substr(0, uuid_position_end_ );
            } else if( device.nt() == ssdp::NS_ROOT_DEVICE ) {
                _rootdevice_usn = device.usn();
            } else if( device.nt() == device.usn() ) {
                _rootdevice_usn = device.usn();
            } else if( squawk::SUAWK_SERVER_DEBUG ) {
                LOG4CXX_DEBUG ( logger, "can not parse usn: " << device.nt() << "=" << device.usn() )
            }

            if( device.nt() == ssdp::NS_ROOT_DEVICE ) {
                if( type == ssdp::SSDPEventListener::BYE ) {
                    if( _ssdp_devices.find( _rootdevice_usn ) != _ssdp_devices.end() ) {
                        if( squawk::SUAWK_SERVER_DEBUG ) {
                            LOG4CXX_DEBUG ( logger, "SSDPEvent (remove rootdevice): " << device )
                        }
                        _ssdp_devices.erase( _ssdp_devices.find( _rootdevice_usn ) );
                    }
                } else if( _ssdp_devices.find( _rootdevice_usn ) != _ssdp_devices.end() ) {
                    //update timestamp
                    if( squawk::SUAWK_SERVER_DEBUG ) {
                        LOG4CXX_TRACE ( logger, "SSDPEvent (existing rootdevice): " << device )
                    }
                     _ssdp_devices[ _rootdevice_usn ].touch();
                } else {
                    //create new rootdevice
                    try {
                        if( squawk::SUAWK_SERVER_DEBUG ) {
                            LOG4CXX_TRACE ( logger, "SSDPEvent (new rootdevice): " << device )
                        }
                        _ssdp_devices[ _rootdevice_usn ] = upnp::deviceDescription( device );
                        _ssdp_devices[ _rootdevice_usn ].touch();
                        _ssdp_devices[ _rootdevice_usn ].timeout( device.cacheControl() );
                    } catch( commons::xml::XmlException & ex ) {
                        LOG4CXX_FATAL ( logger, "XML Parse Exception (" << ex.code() << ") " << ex.what() )
                    } catch( ... ) {
                        LOG4CXX_FATAL ( logger, "Other Exception in parse new rootdevice." )
                    }
                }
            } /* TODO else {
                if( _ssdp_devices.find( _rootdevice_usn ) != _ssdp_devices.end() ) {
                    // std::cout << "SSDPEvent (new endpoint): " << device << std::endl;

                } else { std::cerr << "root device not found: " << _rootdevice_usn << std::endl; }
            } */
        }
    );

    /* clean the devices */
    _ssdp_devices_thread = std::unique_ptr<std::thread> ( new std::thread( [this](){
        while ( announce_thread_run ) {
            //std::lock_guard<std::mutex> _ssdp_devices_guard( _ssdp_devices_mutex );
            for( auto ssdp_device_ : _ssdp_devices ) {
                if( ssdp_device_.second.timeout() ) {
                    if( squawk::SUAWK_SERVER_DEBUG ) {
                        LOG4CXX_DEBUG ( logger, "SSDPEvent Cleanup Thread[Remove Device]:  " << ssdp_device_.first )
                    }
                    _ssdp_devices.erase( _ssdp_devices.find( ssdp_device_.first ) );
                }
            }
            std::this_thread::sleep_for ( std::chrono::milliseconds ( 5000 ) );
        }
    }));

    /* register the upnp CDS servlets. */
    squawk::UpnpContentDirectory * content_directory = new squawk::UpnpContentDirectory("/ctl/ContentDir" );
    content_directory->registerContentDirectoryModule( std::unique_ptr< squawk::ContentDirectoryModule >( new squawk::UpnpContentDirectoryMusic() ) );
    content_directory->registerContentDirectoryModule( std::unique_ptr< squawk::ContentDirectoryModule >( new squawk::UpnpContentDirectoryVideo() ) );
    content_directory->registerContentDirectoryModule( std::unique_ptr< squawk::ContentDirectoryModule >( new squawk::UpnpContentDirectoryImage() ) );
    content_directory->registerContentDirectoryModule( std::unique_ptr< squawk::ContentDirectoryModule >( new squawk::UpnpContentDirectoryFile() ) );

    squawk::UpnpConnectionManager * connection_manager = new squawk::UpnpConnectionManager("/ctl/ConnectionMgr");

    //Setup and start the HTTP server
    web_server = new http::WebServer(
                squawk_config->httpAddress(),
                squawk_config->httpPort() /*,
                squawk_config->int_value(CONFIG_HTTP_THREADS) */ );

    squawk::UpnpXmlDescription * xmldescription = new squawk::UpnpXmlDescription(std::string("/rootDesc.xml") );
    squawk::UpnpContentDirectoryApi * content_directory_api =
            new squawk::UpnpContentDirectoryApi(std::string(
                "/api/(upnp/device|upnp/event|album|artist|track|browse|statistic)/?(\\d*)?"), _upnp_cds_dao, ssdp_server );
    squawk::UpnpMediaServlet * upnp_media_servlet = new squawk::UpnpMediaServlet(
                "/(video|audio|image|cover|albumArtUri|resource)/(\\d*).(flac|mp3|avi|mp4|mkv|mpeg|mov|wmv|jpg)" );

    http::servlet::FileServlet * bowerServlet = new http::servlet::FileServlet(std::string("/bower_components/.*"), squawk_config->bowerRoot());
    http::servlet::FileServlet * fileServlet = new http::servlet::FileServlet(std::string("/.*"), squawk_config->docRoot());

    web_server->register_servlet(content_directory);
    web_server->register_servlet(content_directory_api);
    web_server->register_servlet(connection_manager);
    web_server->register_servlet(xmldescription);
    web_server->register_servlet(upnp_media_servlet);
    web_server->register_servlet(bowerServlet);
    web_server->register_servlet(fileServlet);
    web_server->start();
    ssdp_server->start();

    //rescan the media directory
    if(squawk_config->rescan) {
        _upnp_file_parser->parse( squawk_config->mediaDirectories() );
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5000)); //wait for the server to start
    }

    std::cout << "server started and library rescanned" << std::endl;

    ssdp_server->announce();
    ssdp_server->search();
}
void SquawkServer::stop() {
    std::cout << "stop ssdp_server" << std::endl;
    ssdp_server->stop();
    delete ssdp_server;
  
    std::cout << "stop web_server" << std::endl;
    web_server->stop();
    delete web_server;
}
}//namespace squawk
