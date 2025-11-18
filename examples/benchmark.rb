# frozen_string_literal: true

require "mmap-ruby"

puts RUBY_DESCRIPTION
puts "mmap-ruby version #{MmapRuby::VERSION}"

t1 = Time.now

reader, writer = IO.pipe

fork do
  reader.close
  1_000_000.times do
    writer.write("aa\n")
  end
  writer.write("zz\n")
  writer.close
end

writer.close

line = ""
loop do
  begin
    line = reader.read_nonblock(3)
    break if line == "zz\n"
    raise "Expected 'aa\n', got '#{line}'" unless line == "aa\n"
  rescue IO::WaitReadable
  end
end

reader.close

t2 = Time.now
puts "Time taken for IO.pipe: #{t2 - t1} seconds"

file = File.open("example.txt", "w+")
file.write("\0" * 3_000_003)
file.close

t3 = Time.now

mmap = Mmap.new(file.path, "rw")

fork do
  1_000_000.times do |i|
    mmap[i * 3, 3] = "aa\n"
  end
  mmap[3_000_000, 3] = "zz\n"
end

line = ""
index = 0
loop do
  line = mmap[index, 3]
  index += 3

  if line == "\0\0\0"
    next
  end

  break if line == "zz\n"
  raise "Expected 'aa\n', got '#{line}'" unless line == "aa\n"
end

mmap.unmap

t4 = Time.now
puts "Time taken for Mmap: #{t4 - t3} seconds"

File.delete("example.txt")
