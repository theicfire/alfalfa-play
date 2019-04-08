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

#include <signal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "exception.hh"
#include "pacer.hh"
#include "packet.hh"
#include "poller.hh"
#include "socket.hh"
#include "socketpair.hh"
#include "fecpp.h"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

class AverageEncodingTime {
 private:
  static constexpr double ALPHA = 0.1;

  double value_{-1.0};
  microseconds last_update_{0};

 public:
  void add(const microseconds timestamp_us) {
    assert(timestamp_us >= last_update_);

    if (value_ < 0) {
      value_ = 0;
    } else if (timestamp_us - last_update_ > 1s /* 1 seconds */) {
      value_ = 0;
    } else {
      double new_value = max(0l, duration_cast<microseconds>(timestamp_us - last_update_).count());
      value_ = ALPHA * new_value + (1 - ALPHA) * value_;
    }

    last_update_ = timestamp_us;
  }

  uint32_t int_value() const {
    return static_cast<uint32_t>(value_);
  }
};

size_t target_size(uint32_t avg_delay,
                   const uint64_t last_acked,
                   const uint64_t last_sent,
                   const uint32_t max_delay = 100 * 1000 /* 100 ms = 100,000 us */) {
  if (avg_delay == 0) {
    avg_delay = 1;
  }

  /* cerr << "Packets in flight: " << last_sent - last_acked << "\n";
  cerr << "Avg inter-packet-arrival interval: " << avg_delay << "\n";
  cerr << "Imputed delay: " << avg_delay * (last_sent - last_acked) << "
  us\n"; */

  return 1400 * max(0l, static_cast<int64_t>(max_delay / avg_delay - (last_sent - last_acked)));
}

void usage(const char *argv0) {
  cerr << "Usage: " << argv0 << " PORT" << endl;
}

uint64_t ack_seq_no(const AckPacket &ack, const vector<uint64_t> &cumulative_fpf) {
  return (ack.frame_no() > 0) ? (cumulative_fpf[ack.frame_no() - 1] + ack.fragment_no()) : ack.fragment_no();
}

enum class OperationMode { S1, S2, Conventional };

std::string string_to_hex(const std::string &input) {
  static const char *const lut = "0123456789ABCDEF";
  size_t len = input.length();

  std::string output;
  output.reserve(2 * len);
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = input[i];
    output.push_back(lut[c >> 4]);
    output.push_back(lut[c & 15]);
  }
  return output;
}



using fecpp::byte;

namespace {

class output_checker
   {
   public:
      void operator()(size_t block, size_t /*max_blocks*/,
                      const byte buf[], size_t size)
         {
         for(size_t i = 0; i != size; ++i)
            {
            byte expected = block*size + i;
            if(buf[i] != expected)
               printf("block=%d i=%d got=%02X expected=%02X\n",
                      (int)block, (int)i, buf[i], expected);

            }
         }
   };

class save_to_map
   {
   public:
      save_to_map(size_t& share_len_arg,
                  std::map<size_t, const byte*>& m_arg) :
         share_len(share_len_arg), m(m_arg) {}

      void operator()(size_t block_no, size_t,
                      const byte share[], size_t len)
         {
         share_len = len;

         // Contents of share[] are only valid in this scope, must copy
         byte* share_copy = new byte[share_len];
         memcpy(share_copy, share, share_len);
         m[block_no] = share_copy;
         }
   private:
      size_t& share_len;
      std::map<size_t, const byte*>& m;
   };

void benchmark_fec(size_t k, size_t n)
   {

   fecpp::fec_code fec(k, n);

   std::vector<byte> input(k * 1024);
   for(size_t i = 0; i != input.size(); ++i)
      input[i] = i;

   std::map<size_t, const byte*> shares;
   size_t share_len;

   save_to_map saver(share_len, shares);

   fec.encode(&input[0], input.size(), std::tr1::ref(saver));

   while(shares.size() > k)
      shares.erase(shares.begin());

   output_checker check_output;
   fec.decode(shares, share_len, check_output);
   }

}

void run_fec_stuff() {
   const int Ms[] = {1, 2, 3, 4, 5, 7, 8, 16, 32, 64, 128, 0 };
   const int Ks[] = {1,2,3,4,5,6,7,8,9,10,15,16,17,20,31,32,33,63,64,65,127 ,0};

   for(int i = 0; Ms[i]; ++i)
      {
      for(int j = 0; Ks[j]; ++j)
         {
         int k = Ks[j];
         int m = Ms[i];

         if(k > m)
            continue;

         printf("%d %d\n", k, m);
         benchmark_fec(k, m);
         }
      }
}






int main(int argc, char *argv[]) {
  /* check the command-line arguments */
  if (argc < 1) { /* for sticklers */
    abort();
  }

  if (argc < 2) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  int port = atoi(argv[1]);

  run_fec_stuff();

  vector<uint8_t> frame;
  for (int i = 0; i < 80 * 1360; i++) {
    frame.push_back(i % 256);
  }

  while (true) {
    /* construct Socket for outgoing datagrams */
    UDPSocket socket;
    socket.bind(Address("0", port));
    socket.set_timestamps();
    uint32_t frame_no = 0;
    system_clock::time_point last_sent = system_clock::now();
    uint16_t connection_id = 1337;

    cout << "Waiting for a receiver to send a message at port " << port << endl;
    const auto new_fragment = socket.recv();
    socket.connect(new_fragment.source_address);
    cout << "Done receiving" << endl;
    /* handle events */
    while (true) {
      FragmentedFrame ff{connection_id,
                         1,
                         1,
                         frame_no,
                         static_cast<uint32_t>(duration_cast<microseconds>(system_clock::now() - last_sent).count()),
                         frame};
      cout << "Sending frame " << frame_no << " with " << ff.packets().size() << " packets" << endl;
      bool no_error = true;
      for (const auto &packet : ff.packets()) {
        string packet_string = packet.to_string();

        // cout << "\nSending packet " << string_to_hex(packet_string) <<
        // endl;
        try {
          socket.send(packet.to_string());
        } catch (const std::exception &e) {  // reference to the base of a polymorphic object
          cout << "Failed to send" << endl;
          no_error = false;
          break;
        }
      }
      if (!no_error) {
        break;
      }
      last_sent = system_clock::now();
      frame_no += 1;
      // for (int i = 0; i < 160; i++) {
      //   frame_no += 1;
      //   if (count % 100 == 0) {
      //     cout << "Sending " << frame_no << endl;
      //   }
      //   char buffer[1400];
      //   memset(buffer, 'X', 1400);

      //   sprintf(buffer + 10, "%d", frame_no);
      //   string msg = buffer;
      //   socket.send(msg);
      //   // usleep(100);
      // }

      usleep(40000);
    }
  }

  return EXIT_FAILURE;
}
