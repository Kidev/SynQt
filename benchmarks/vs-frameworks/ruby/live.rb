# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Ruby's column of the live-path comparison: Action Cable on puma, with the N subscribers as
# WebSocket clients in the same process. The same shape as node/live-bare.mjs, deliberately,
# so a reader can put the two files next to each other and see that the only difference is
# the runtime.
#
# Action Cable rather than a bare WebSocket, because Action Cable is what a Rails team
# reaches for when the server has to push. The table already has two floor columns (go-bare
# and rust-bare) and does not need a third.
#
# Publisher and subscribers share one process, as they do in every other column, so this
# times an interval on one monotonic clock rather than across two.
#
# The subscribers are fibers on ONE thread, under the Async scheduler, and not a thread each.
# That is load-bearing rather than stylistic. Ruby's threads are real OS threads under a
# global VM lock, and N of them each waking on a socket read is a queue: at N=50 and 30 Hz
# the thread version of this file reported p50 198-409 ms and dropped frames, while the same
# server, protocol and rate with fiber subscribers reports 1.2 ms and loses nothing. Python
# asyncio subscribers against this same server agree with the fiber number to within 0.3 ms,
# which is what says the server was never the slow part and the threads were.
#
# What is still true and is what the README says where the number is reported: the Ruby part
# of this column runs on one core, so read the CPU row as one core's worth of Ruby rather
# than as a per-core figure.
#
# See COLUMN-CONTRACT.md.

require "concurrent"
# Action Cable's subscription adapter builds a Concurrent::TimerTask, and concurrent-ruby
# stopped loading that from its top-level require: without this line every /cable upgrade
# fails with "uninitialized constant Concurrent::TimerTask" and no subscriber ever comes up.
require "concurrent/timer_task"

require "action_cable"
require "async"
require "async/http/endpoint"
require "async/websocket/client"
require "base64"
require "json"
require "puma"
require "rack"
require "socket"

# The process's monotonic origin. Every stamp is microseconds since this, on both ends, so
# what is reported is an interval. CLOCK_MONOTONIC and never Time.now, which would put a
# clock step into the distribution as if it were a propagation time.
ORIGIN = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)

def now_micros
  Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond) - ORIGIN
end

# Resident set size as the OS reports it, which is what every other column reports. Ruby's
# ObjectSpace accounting is a different measurement and does not go in this field.
def resident_bytes
  field = File.read("/proc/self/statm").split[1]
  field.to_i * 4096
rescue StandardError
  0
end

# Process CPU in milliseconds, user plus system: the figure a host is sized on, and the one
# every other column reports.
def cpu_milliseconds
  times = Process.times
  (times.utime + times.stime) * 1000
end

# Linear interpolation between ranks, exactly as measure.mjs does it. A nearest-rank
# percentile would disagree with every other column at small sample counts, which is the kind
# of difference that reads as a fact about the runtime.
def summarize(samples)
  sorted = samples.sort
  at = lambda do |fraction|
    next 0.0 if sorted.empty?

    rank = fraction * (sorted.length - 1)
    low = rank.floor
    high = rank.ceil
    next sorted[low] if low == high

    sorted[low] + (rank - low) * (sorted[high] - sorted[low])
  end
  {
    "unit" => "ms",
    "samples" => sorted.length,
    "min" => sorted.first || 0.0,
    "p50" => at.call(0.5),
    "p95" => at.call(0.95),
    "p99" => at.call(0.99),
    "max" => sorted.last || 0.0,
    "mean" => sorted.empty? ? 0.0 : sorted.sum / sorted.length
  }
end

# `--flag value` and bare `--flag`, the same shape measure.mjs parses, so one set of
# arguments from run-bench.sh means the same thing to every column.
def parse_args(defaults)
  values = defaults.dup
  index = 0
  while index < ARGV.length
    flag = ARGV[index]
    index += 1
    next unless flag.start_with?("--")

    name = flag[2..]
    value = ARGV[index]
    if value.nil? || value.start_with?("--")
      values[name] = "true"
      next
    end
    values[name] = value
    index += 1
  end
  values
end

# The channel every subscriber joins. It carries no actions a client calls: in this workload
# subscribers never send, they only receive.
class LiveChannel < ActionCable::Channel::Base
  def subscribed
    stream_from "live"
  end
end

class LiveConnection < ActionCable::Connection::Base
end

args = parse_args(
  "subscribers" => "10,50,100,250",
  "seconds" => "5",
  "hz" => "30",
  "payload" => "256",
  "saturate" => "false",
  "out" => ""
)
sizes = args["subscribers"].split(",").map { |part| part.strip.to_i }.select(&:positive?)
seconds = [1, args["seconds"].to_i].max
hz = [1, args["hz"].to_i].max
payload_bytes = [0, args["payload"].to_i].max
saturate = args["saturate"] == "true"

puts "Action Cable live path: #{seconds}s at " \
     "#{saturate ? 'saturation' : "#{hz} Hz"}, #{payload_bytes} byte payload, " \
     "subscribers #{args['subscribers']}"
$stdout.flush

server = ActionCable::Server::Base.new
server.config.cable = { "adapter" => "async" }
server.config.connection_class = -> { LiveConnection }
server.config.disable_request_forgery_protection = true
server.config.logger = Logger.new(IO::NULL)
ActionCable::Server::Base.class_variable_set(:@@config, server.config) if
  ActionCable::Server::Base.class_variable_defined?(:@@config)
Object.const_set(:ApplicationCable, Module.new) unless defined?(ApplicationCable)

listener = TCPServer.new("127.0.0.1", 0)
port = listener.addr[1]
listener.close

app = Rack::Builder.new do
  map "/cable" do
    run server
  end
  run ->(_env) { [426, { "content-type" => "text/plain" }, ["upgrade required"]] }
end.to_app

puma = Puma::Server.new(app)
puma.add_tcp_listener("127.0.0.1", port)
puma_thread = Thread.new { puma.run.join }
sleep 0.2

payload = "x" * payload_bytes
baseline_rss = resident_bytes

# The whole sweep runs inside one Async reactor: the subscribers are fibers on this thread
# and so is the publisher's pacing, so there is exactly one Ruby thread doing application
# work. Puma keeps its own threads for the server half, as every column's server does.
sweep = Sync do |task|
  endpoint = Async::HTTP::Endpoint.parse("http://127.0.0.1:#{port}/cable")

  sizes.map do |subscriber_count|
    measuring = false
    delivered_this_frame = 0
    joined = 0
    fleet = nil
    # One sample buffer per subscriber, appended by that subscriber's own fiber. They all
    # run on one thread, so nothing contends and no lock is needed anywhere below.
    buffers = Array.new(subscriber_count) { [] }

    subscribers =
      Array.new(subscriber_count) do |index|
        mine = buffers[index]
        task.async do
          Async::WebSocket::Client.connect(endpoint) do |socket|
            while (message = socket.read)
              envelope =
                begin
                  JSON.parse(message.buffer)
                rescue JSON::ParserError
                  next
                end

              case envelope["type"]
              when "welcome"
                socket.write(Protocol::WebSocket::TextMessage.generate(
                  { command: "subscribe",
                    identifier: { channel: "LiveChannel" }.to_json }
                ))
                socket.flush
              when "confirm_subscription"
                joined += 1
              when nil
                next unless measuring

                # Action Cable's envelope is JSON, so the 8-byte stamp travels base64
                # inside it: the frame layout is the contract's, and the encoding around it
                # is what Action Cable actually puts on the wire. The README says so.
                frame = Base64.strict_decode64(envelope["message"].to_s)
                next if frame.bytesize < 8

                stamp = frame.byteslice(0, 8).unpack1("Q<")
                mine << ((now_micros - stamp) / 1000.0)
                delivered_this_frame += 1
                fleet&.resume if delivered_this_frame >= subscriber_count
              end
            end
          end
        end
      end

    deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 30
    while joined < subscriber_count && Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
      task.sleep 0.01
    end
    if joined < subscriber_count
      warn "subscribers did not come up at N=#{subscriber_count}"
      exit 1
    end
    connected_rss = resident_bytes

    publish = lambda do
      frame = [now_micros].pack("Q<") + payload
      server.broadcast("live", Base64.strict_encode64(frame))
    end

    # Warm up before measuring, for the same reason every other column does: the first
    # frames pay for lazily built buffers on both ends and would otherwise be the whole
    # tail. `measuring` is still false, so none of it reaches the statistics.
    [hz, 30].min.times do
      publish.call
      task.sleep 1.0 / hz
    end
    measuring = true

    ticks = 0
    cpu_before = cpu_milliseconds
    started_at = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    if saturate
      # Closed loop: publish, wait for the whole fleet to have it, publish again.
      # Open-looping at "maximum rate" would measure the send buffer rather than the
      # capacity, and the SynQt column cannot open-loop at all, so this is the mode every
      # column shares.
      while Process.clock_gettime(Process::CLOCK_MONOTONIC) - started_at < seconds
        delivered_this_frame = 0
        publish.call
        ticks += 1
        landed = false
        guard = task.async do
          task.sleep 5
          landed = false
          fleet = nil
        end
        if delivered_this_frame < subscriber_count
          fleet = Fiber.current
          Fiber.yield
        end
        landed = delivered_this_frame >= subscriber_count
        fleet = nil
        guard.stop
        unless landed
          warn "a frame never reached every subscriber at N=#{subscriber_count}"
          exit 1
        end
      end
    else
      ticks = seconds * hz
      ticks.times do |tick|
        publish.call
        due = started_at + ((tick + 1).to_f / hz)
        wait = due - Process.clock_gettime(Process::CLOCK_MONOTONIC)
        task.sleep wait if wait.positive?
      end
      # Let what is in flight land, or the tail of every run reads as loss that is really
      # the harness stopping first.
      task.sleep 0.5
    end
    elapsed_seconds = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started_at
    cpu_ms = cpu_milliseconds - cpu_before
    measuring = false

    propagation = buffers.flatten
    delivered = propagation.length
    entry = {
      "subscribers" => subscriber_count,
      "propagation" => summarize(propagation),
      "throughput_msgs_per_sec" => delivered / [elapsed_seconds, 0.001].max,
      "cpu_ms_per_1k" => delivered.positive? ? (cpu_ms * 1000) / delivered : 0.0,
      "rss_bytes_per_conn" => (subscriber_count.positive? && connected_rss > baseline_rss ?
        (connected_rss - baseline_rss).to_f / subscriber_count : 0.0),
      "rss_total_bytes" => connected_rss,
      "delivered" => delivered,
      "expected" => ticks * subscriber_count
    }
    puts format("  N=%d  p50 %.3f ms  p99 %.3f ms  %.0f msg/s  delivered %d/%d",
                entry["subscribers"], entry["propagation"]["p50"],
                entry["propagation"]["p99"], entry["throughput_msgs_per_sec"],
                entry["delivered"], entry["expected"])
    $stdout.flush

    subscribers.each(&:stop)
    task.sleep 0.2
    entry
  end
end

puma.stop(true)
puma_thread.kill

document = {
  "benchmark" => "vs-frameworks-live",
  "stack" => "ruby-actioncable",
  "path" => "Action Cable on puma, JSON envelope over WebSockets",
  "ruby_version" => RUBY_VERSION,
  # "linux <kernel release>", the same shape measure.mjs writes: the table's header prints
  # one column's host for the whole run, so two columns that named the machine differently
  # would make it a lottery which one it read.
  "host" => "linux #{File.read('/proc/sys/kernel/osrelease').strip}",
  "arch" => RbConfig::CONFIG["host_cpu"],
  "recorded" => Time.now.utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
  "rss_available" => true,
  "hz" => saturate ? 0 : hz,
  "saturated" => saturate,
  "seconds" => seconds,
  "payload_bytes" => payload_bytes,
  "sweep" => sweep
}

unless args["out"].to_s.empty?
  File.write(args["out"], "#{JSON.pretty_generate(document)}\n")
  puts "\nwrote #{args['out']}"
end
