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

#include "camera.hh"
#include "encoder.hh"
#include "exception.hh"
#include "finally.hh"
#include "pacer.hh"
#include "packet.hh"
#include "paranoid.hh"
#include "poller.hh"
#include "procinfo.hh"
#include "socket.hh"
#include "socketpair.hh"
#include "yuv4mpeg.hh"

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
            double new_value =
                max(0l, duration_cast<microseconds>(timestamp_us - last_update_)
                            .count());
            value_ = ALPHA * new_value + (1 - ALPHA) * value_;
        }

        last_update_ = timestamp_us;
    }

    uint32_t int_value() const { return static_cast<uint32_t>(value_); }
};

struct EncodeJob {
    string name;

    RasterHandle raster;

    Encoder encoder;
    EncoderMode mode;

    uint8_t y_ac_qi;
    size_t target_size;

    EncodeJob(const string &name, RasterHandle raster, const Encoder &encoder,
              const EncoderMode mode, const uint8_t y_ac_qi,
              const size_t target_size)
        : name(name),
          raster(raster),
          encoder(encoder),
          mode(mode),
          y_ac_qi(y_ac_qi),
          target_size(target_size) {}
};

struct EncodeOutput {
    Encoder encoder;
    vector<uint8_t> frame;
    uint32_t source_minihash;
    milliseconds encode_time;
    string job_name;
    uint8_t y_ac_qi;

    EncodeOutput(Encoder &&encoder, vector<uint8_t> &&frame,
                 const uint32_t source_minihash, const milliseconds encode_time,
                 const string &job_name, const uint8_t y_ac_qi)
        : encoder(move(encoder)),
          frame(move(frame)),
          source_minihash(source_minihash),
          encode_time(encode_time),
          job_name(job_name),
          y_ac_qi(y_ac_qi) {}
};

EncodeOutput do_encode_job(EncodeJob &&encode_job) {
    vector<uint8_t> output;

    uint32_t source_minihash = encode_job.encoder.minihash();

    const auto encode_beginning = system_clock::now();

    uint8_t quantizer_in_use = 0;

    switch (encode_job.mode) {
        case CONSTANT_QUANTIZER:
            output = encode_job.encoder.encode_with_quantizer(
                encode_job.raster.get(), encode_job.y_ac_qi);
            quantizer_in_use = encode_job.y_ac_qi;
            break;

        case TARGET_FRAME_SIZE:
            output = encode_job.encoder.encode_with_target_size(
                encode_job.raster.get(), encode_job.target_size);
            break;

        default:
            throw runtime_error("unsupported encoding mode.");
    }

    const auto encode_ending = system_clock::now();
    const auto ms_elapsed =
        duration_cast<milliseconds>(encode_ending - encode_beginning);

    return {move(encode_job.encoder), move(output),
            source_minihash,          ms_elapsed,
            encode_job.name,          quantizer_in_use};
}

size_t target_size(uint32_t avg_delay, const uint64_t last_acked,
                   const uint64_t last_sent,
                   const uint32_t max_delay = 100 *
                                              1000 /* 100 ms = 100,000 us */) {
    if (avg_delay == 0) {
        avg_delay = 1;
    }

    /* cerr << "Packets in flight: " << last_sent - last_acked << "\n";
    cerr << "Avg inter-packet-arrival interval: " << avg_delay << "\n";
    cerr << "Imputed delay: " << avg_delay * (last_sent - last_acked) << "
    us\n"; */

    return 1400 * max(0l, static_cast<int64_t>(max_delay / avg_delay -
                                               (last_sent - last_acked)));
}

void usage(const char *argv0) {
    cerr
        << "Usage: " << argv0
        << " [-m,--mode MODE] [-d, --device CAMERA] [-p, --pixfmt PIXEL_FORMAT]"
        << " [-u,--update-rate RATE] [--log-mem-usage] HOST PORT CONNECTION_ID"
        << endl
        << endl
        << "Accepted MODEs are s1, s2 (default), conventional." << endl;
}

uint64_t ack_seq_no(const AckPacket &ack,
                    const vector<uint64_t> &cumulative_fpf) {
    return (ack.frame_no() > 0)
               ? (cumulative_fpf[ack.frame_no() - 1] + ack.fragment_no())
               : ack.fragment_no();
}

enum class OperationMode { S1, S2, Conventional };

int main(int argc, char *argv[]) {
    /* check the command-line arguments */
    if (argc < 1) { /* for sticklers */
        abort();
    }

    if (argc < 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* construct Socket for outgoing datagrams */
    UDPSocket socket;
    socket.connect(Address(argv[1], argv[2]));
    socket.set_timestamps();

    /* make pacer to smooth out outgoing packets */
    Pacer pacer;

    /* get connection_id */
    const uint16_t connection_id = paranoid::stoul(argv[3]);

    /* average inter-packet delay, reported by receiver */
    uint32_t avg_delay = numeric_limits<uint32_t>::max();

    /* keep the number of fragments per frame */
    vector<uint64_t> cumulative_fpf;
    uint64_t last_acked = numeric_limits<uint64_t>::max();

    /* latest raster that is received from the input */
    Optional<RasterHandle> last_raster;

    /* keep the moving average of encoding times */
    AverageEncodingTime avg_encoding_time;

    /* decoder hash => encoder object */
    deque<uint32_t> encoder_states;

    /* latest state of the receiver, based on ack packets */
    Optional<uint32_t> receiver_last_acked_state;
    Optional<uint32_t> receiver_assumed_state;
    deque<uint32_t> receiver_complete_states;

    /* if the receiver goes into an invalid state, for this amount of seconds,
       we will go into a conservative mode: we only encode based on a known
       state */
    seconds conservative_for{5};

    /* comment */
    auto make_magic_happen = UnixDomainSocket::make_pair();

    Poller poller;

    /* all encode jobs have finished */
    poller.add_action(
        Poller::Action(make_magic_happen.second, Direction::In, [&]() {
            printf("Hello, I got a message\n");
            string output = make_magic_happen.second.read();
            printf("This is the poller message %s\n", output.c_str());

            string msg = "hello";
            pacer.push(msg, 2000u);

            return ResultType::Continue;
        }));

    /* new ack from receiver */
    poller.add_action(Poller::Action(socket, Direction::In, [&]() {
        auto packet = socket.recv();
        AckPacket ack(packet.payload);

        if (ack.connection_id() != connection_id) {
            /* this is not an ack for this session! */
            return ResultType::Continue;
        }

        uint64_t this_ack_seq = ack_seq_no(ack, cumulative_fpf);

        if (last_acked != numeric_limits<uint64_t>::max() and
            this_ack_seq < last_acked) {
            /* we have already received an ACK newer than this */
            return ResultType::Continue;
        }

        last_acked = this_ack_seq;
        avg_delay = ack.avg_delay();
        receiver_last_acked_state.reset(ack.current_state());
        receiver_complete_states = move(ack.complete_states());

        return ResultType::Continue;
    }));

    /* outgoing packet ready to leave the pacer */
    poller.add_action(
        Poller::Action(socket, Direction::Out,
                       [&]() {
                           assert(pacer.ms_until_due() == 0);

                           while (pacer.ms_until_due() == 0) {
                               assert(not pacer.empty());

                               socket.send(pacer.front());
                               pacer.pop();
                           }

                           return ResultType::Continue;
                       },
                       [&]() { return pacer.ms_until_due() == 0; }));

    /* kick off the first encode */
    make_magic_happen.first.write("1");

    /* handle events */
    while (true) {
        const auto poll_result = poller.poll(pacer.ms_until_due());
        if (poll_result.result == Poller::Result::Type::Exit) {
            if (poll_result.exit_status) {
                cerr << "Connection error." << endl;
            }

            return poll_result.exit_status;
        }
    }

    return EXIT_FAILURE;
}
