/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

/* Copyright 2013-2018 the Alfalfa authors
                       and the Massachusetts Institute of Technology

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

      1. Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.

      2. Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <getopt.h>

#include <cstdlib>
#include <random>
#include <unordered_map>
#include <utility>
#include <tuple>
#include <queue>
#include <deque>
#include <thread>
#include <condition_variable>
#include <future>

#include "socket.hh"
#include "packet.hh"
#include "poller.hh"
#include "optional.hh"
#include "procinfo.hh"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

class AverageInterPacketDelay
{
private:
  static constexpr double ALPHA = 0.1;

  double value_ { -1.0 };
  uint64_t last_update_{ 0 };

public:
  uint32_t add( const uint64_t timestamp_us, const int32_t grace )
  {
    assert( timestamp_us >= last_update_ );
    uint32_t ret = max( 0l, static_cast<int64_t>( timestamp_us - last_update_ - grace ) );
    

    if ( value_ < 0 ) {
      value_ = 0;
    }
    // else if ( timestamp_us - last_update_ > 0.2 * 1000 * 1000 /* 0.2 seconds */ ) {
    //   value_ /= 4;
    // }
    else {
      double new_value = max( 0l, static_cast<int64_t>( timestamp_us - last_update_ - grace ) );
      value_ = ALPHA * new_value + ( 1 - ALPHA ) * value_;
    }

    last_update_ = timestamp_us;
    return ret;
  }

  uint32_t int_value() const { return static_cast<uint32_t>( value_ ); }
};

void usage( const char *argv0 )
{
  cerr << "Usage: " << argv0 << " [-f, --fullscreen] [--verbose] PORT WIDTH HEIGHT" << endl;
}

uint16_t ezrand()
{
  random_device rd;
  uniform_int_distribution<uint16_t> ud;

  return ud( rd );
}

int main( int argc, char *argv[] )
{
  /* check the command-line arguments */
  if ( argc < 1 ) { /* for sticklers */
    abort();
  }

  const option command_line_options[] = {
    { "fullscreen", no_argument, nullptr, 'f' },
    { "verbose",    no_argument, nullptr, 'v' },
    { 0, 0, 0, 0 }
  };

  while ( true ) {
    const int opt = getopt_long( argc, argv, "f", command_line_options, nullptr );

    if ( opt == -1 ) {
      break;
    }

    switch ( opt ) {
    case 'f':
      break;

    case 'v':
      break;

    default:
      usage( argv[ 0 ] );
      return EXIT_FAILURE;
    }
  }

  if ( optind + 2 >= argc ) {
    usage( argv[ 0 ] );
    return EXIT_FAILURE;
  }

  /* choose a random connection_id */
  const uint16_t connection_id = 1337; // ezrand();
  cerr << "Connection ID: " << connection_id << endl;

  /* construct Socket for incoming  datagrams */
  UDPSocket socket;
  socket.connect(Address("127.0.0.1", "9000"));
  socket.set_timestamps();

  /* frame no => FragmentedFrame; used when receiving packets out of order */
  unordered_map<size_t, FragmentedFrame> fragmented_frames;

  /* EWMA */
  AverageInterPacketDelay avg_delay;
  // size_t next_frame_no = 0;
  deque<uint32_t> complete_states;
  uint32_t expected_packet_no = 0;

  Poller poller;
  poller.add_action( Poller::Action( socket, Direction::In,
    [&]()
    {
      /* wait for next UDP datagram */
      const auto new_fragment = socket.recv();
      const Packet packet { new_fragment.payload };
      // cout << packet.frame_no() * packet.fragments_in_this_frame() + packet.fragment_no() << "\t" << expected_packet_no << endl;
      expected_packet_no++;

      uint32_t delay = avg_delay.add( new_fragment.timestamp_us, packet.time_since_last() );
      cout << expected_packet_no << "\t" << delay << endl;

      // if ( packet.frame_no() < next_frame_no ) {
      //   /* we're not interested in this anymore */
      //   cout << "skip frame.. too old" << endl;
      //   return ResultType::Continue;
      // }
      // else if ( packet.frame_no() > next_frame_no ) {
      //   /* current frame is not finished yet, but we just received a packet
      //      for the next frame, so here we just encode the partial frame and
      //      display it and move on to the next frame */
      //   cout << "got a packet for future frame #" << packet.frame_no() << endl;
             
      //   next_frame_no = packet.frame_no();
      // }
 
      // /* add to current frame */
      // if ( fragmented_frames.count( packet.frame_no() ) ) {
      //   fragmented_frames.at( packet.frame_no() ).add_packet( packet );
      // } else {

      //   fragmented_frames.insert( make_pair( packet.frame_no(),
      //                                        FragmentedFrame( connection_id, packet ) ) );
      // }

      // if ( fragmented_frames.count( next_frame_no ) > 0 and fragmented_frames.at( next_frame_no ).complete() ) {
      //   cout << "Got a full packet #" << next_frame_no << endl;
      //   next_frame_no += 1;
      // }

      // AckPacket( connection_id, packet.frame_no(), packet.fragment_no(),
      //           avg_delay.int_value(), (uint32_t) 22,
      //           complete_states).sendto( socket, new_fragment.source_address );


      // printf("Got a fragment: %s\n", new_fragment.payload.c_str());

      return ResultType::Continue;
    },
    [&]() { return not socket.eof(); } )
  );

  string hi = "hello";
  socket.send(hi);
  /* handle events */
  while ( true ) {
    const auto poll_result = poller.poll( -1 );
    if ( poll_result.result == Poller::Result::Type::Exit ) {
      return poll_result.exit_status;
    }
  }

  return EXIT_SUCCESS;
}
