# frozen_string_literal: true

require "mmap-ruby"

PAGESIZE = 4096

file = File.open("aa", "w")
file.write("\0" * PAGESIZE)
file.write("test")
file.write("\0" * PAGESIZE)
file.close

mmap = Mmap.new("aa", "w", offset: 0)
p mmap.size == "test".size + (2 * PAGESIZE)
p mmap.scan(/[a-z.]+/) == ["test"]
p mmap.index("test") == PAGESIZE
p mmap.rindex("test") == PAGESIZE
p mmap.sub!(/[a-z.]+/, "toto") == mmap
p mmap.scan(/[a-z.]+/) == ["toto"]
begin
  mmap.sub!(/[a-z.]+/, "alpha")
  puts "not OK, must give an error"
rescue
  puts "OK: #$!"
end
mmap.munmap

mmap = Mmap.new("aa", "rw")
p mmap.index("toto") == PAGESIZE
p mmap.sub!(/([a-z.]+)/, "alpha") == mmap
p $& == "toto"
p $1 == "toto"
p mmap.index("toto") == nil
p mmap.index("alpha") == PAGESIZE
p mmap.size == 5 + 2 * PAGESIZE
mmap.gsub!(/\0/, "X")
p mmap.size == 5 + 2 * PAGESIZE

File.delete("aa")
