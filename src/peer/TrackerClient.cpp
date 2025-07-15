#include "VeritasSync/TrackerClient.h"

#include <boost/asio.hpp>
#include <iostream>
#include <sstream>

using boost::asio::ip::tcp;

namespace VeritasSync {

TrackerClient::TrackerClient(std::string host, unsigned short port)
    : m_host(std::move(host)), m_port(port) {}

std::vector<std::string> TrackerClient::register_and_query(
    const std::string& sync_key, unsigned short p2p_port) {
  std::vector<std::string> peers;
  try {
    boost::asio::io_context io_context;
    tcp::socket socket(io_context);
    tcp::resolver resolver(io_context);

    boost::asio::connect(socket,
                         resolver.resolve(m_host, std::to_string(m_port)));


    std::string request =
        "REGISTER " + sync_key + " " + std::to_string(p2p_port) + "\n";
    boost::asio::write(socket, boost::asio::buffer(request));

    boost::asio::streambuf response_buf;
    boost::system::error_code ec;


    boost::asio::read(socket, response_buf, ec);

    if (ec && ec != boost::asio::error::eof) {
      throw boost::system::system_error(ec);
    }

    std::istream response_stream(&response_buf);
    std::string peer_address;
    while (std::getline(response_stream, peer_address)) {
      if (!peer_address.empty()) {
        peers.push_back(peer_address);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "TrackerClient Error: " << e.what() << std::endl;
  }

  return peers;
}

}