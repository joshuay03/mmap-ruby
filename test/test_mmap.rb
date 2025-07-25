# frozen_string_literal: true

require "test_helper"

class TestMmap < Minitest::Test
  EXT_DIR = File.expand_path(File.join(File.dirname(__FILE__), "..", "ext", "mmap_ruby"))

  def setup
    @tmp = Dir.tmpdir
    FileUtils.cp(File.join(EXT_DIR, "mmap_ruby.c"), @tmp)

    @mmap_c = File.join(@tmp, "mmap_ruby.c")

    @mmap = Mmap.new(@mmap_c, "rw")
    @str  = File.read @mmap_c
  end

  def teardown
    @mmap.unmap
    aa = File.join(@tmp, "aa")
    bb = File.join(@tmp, "bb")
    FileUtils.rm(aa) if File.exist?(aa)
    FileUtils.rm(bb) if File.exist?(bb)
  end

  def test_anonymous
    @mmap = Mmap.new(nil, length: 8192, increment: 1024, initialize: " ")
    assert_kind_of(Mmap, @mmap)
    @str = " " * 8192
    1024.times do
      pos = rand(8192)
      @mmap[pos] = @str[pos] = (32 + rand(64)).chr
    end
    assert_equal(@mmap.to_str, @str, "insert anonymous")
    assert_raises(IndexError) { @mmap[12345] = "a" }
    assert_raises(TypeError) { @mmap << "a" }
  end

  def test_fileno
    @mmap = Mmap.new(File.new(@mmap_c, "r+"), "rw")
    test_aref
    @mmap[12] = "3"
    @str[12] = "3"
    assert_equal(@mmap.to_str, @str, "insert io")
    assert_equal(0, @mmap <=> @str, "cmp")
    assert_raises(TypeError) { @mmap[12] = "ab" }
    @mmap.freeze
    if @str.respond_to?(:match)
      assert_equal(@str.match("rb_match_busy").offset(0),
                   @mmap.match("rb_match_busy").offset(0), "match")
      assert_equal(@str.match(/rb_../).offset(0),
                   @mmap.match(/rb_../).offset(0), "match")
      assert_same_result(@str.match("rb_match_buzy"),
                         @mmap.match("rb_match_buzy"), "no match")
      assert_equal(@str =~ /rb_match_busy/,
                   @mmap =~ /rb_match_busy/, "match")
      assert_same_result(@str =~ /rb_match_buzy/,
                         @mmap =~ /rb_match_buzy/, "no match")
    end
    assert_raises(RuntimeError) { @mmap[12] = "a" }
  end

  def test_length
    assert_equal(@mmap.length, @str.length, "<lenght>")
  end

  def test_inspect
    assert @mmap.inspect
  end

  def test_aref
    max = @str.size * 2
    72.times do
      ran1 = rand(max)
      assert_same_result(@str[ran1], @mmap[ran1], "<aref>")
      assert_same_result(@str[-ran1], @mmap[-ran1], "<aref>")
      ran2 = rand(max)
      assert_same_result(@str[ran1, ran2], @mmap[ran1, ran2], "<double aref>")
      assert_same_result(@str[-ran1, ran2], @mmap[-ran1, ran2], "<double aref>")
      assert_same_result(@str[ran1, -ran2], @mmap[ran1, -ran2], "<double aref>")
      assert_same_result(@str[-ran1, -ran2], @mmap[-ran1, -ran2], "<double aref>")
      assert_same_result(@str[ran1..ran2], @mmap[ran1..ran2], "<double aref>")
      assert_same_result(@str[-ran1..ran2], @mmap[-ran1..ran2], "<double aref>")
      assert_same_result(@str[ran1..-ran2], @mmap[ran1..-ran2], "<double aref>")
      assert_same_result(@str[-ran1..-ran2], @mmap[-ran1..-ran2], "<double aref>")
    end
    assert_same_result(@str[/random/], @mmap[/random/], "<aref regexp>")
    assert_same_result(@str[/real/], @mmap[/real/], "<aref regexp>")
    assert_same_result(@str[/none/], @mmap[/none/], "<aref regexp>")
  end

  def test_aset
    @mmap[/...../] = "change it"
    @str[/...../] = "change it"
    assert_equal(@mmap.to_str, @str, "aset regexp")
    @mmap["ge i"] = "change it"
    @str["ge i"] = "change it"
    assert_equal(@mmap.to_str, @str, "aset regexp")
    max = @str.size * 2
    72.times do
      ran1 = rand(max)
      internal_aset(ran1)
      internal_aset(-ran1)
      ran2 = rand(max)
      internal_aset(ran1, ran2)
      internal_aset(ran1, -ran2)
      internal_aset(-ran1, ran2)
      internal_aset(-ran1, -ran2)
      internal_aset(ran1, ran2, false)
      internal_aset(ran1, -ran2, false)
      internal_aset(-ran1, ran2, false)
      internal_aset(-ran1, -ran2, false)
    end
  end

  def test_simple_aref
    assert_equal(@str[10], @mmap[10], "<aref>")
  end

  def test_slice
    max = @str.size * 2
    72.times do
      ran1 = rand(max)
      internal_slice(ran1)
      internal_slice(-ran1)
      ran2 = rand(max)
      internal_slice(ran1, ran2)
      internal_slice(ran1, -ran2)
      internal_slice(-ran1, ran2)
      internal_slice(-ran1, -ran2)
      internal_slice(ran1, ran2, false)
      internal_slice(ran1, -ran2, false)
      internal_slice(-ran1, ran2, false)
      internal_slice(-ran1, -ran2, false)
    end
  end

  def test_concat
    [@mmap, @str].each { |l| l << "bc"; l << 12; l << "ab" }
    assert_equal(@mmap.to_str, @str, "<<")
    assert_raises(TypeError) { @mmap << 456 }
  end

  def test_insert_integer
    mmap = @mmap
    s = mmap.size
    mmap.insert(0, 1.chr)
    assert_equal(s+1, mmap.size)
    assert_equal(1, mmap[0].ord)

    mmap[0,4] = "\x01\x01\x01\x01"
    assert_equal("\x01", mmap[0])
    mmap[0] = 0
    assert_equal("\x00", mmap[0])
    mmap[0] = 1
    assert_equal("\x01", mmap[0])
  end

  def test_extend
    @mmap.extend(4096)
    assert_equal(@mmap.to_str, @str, "extend")
    if @str.respond_to?(:insert)
      10.times do
        pos = rand(@mmap.size)
        str = "XX" * rand(66)
        @str.insert(pos, str)
        @mmap.insert(pos, str)
        assert_equal(@mmap.to_str, @str, "insert")
      end
    end
  end

  def test_each
    assert_equal @str.bytes, @mmap.to_a
  end

  def test_each_line
    actual = []
    expected = []
    @mmap.each_line { |line| actual << line }
    @str.each_line { |line| expected << line }
    assert_equal expected, actual

    actual = []
    expected = []
    @mmap.each_line("in") { |line| actual << line }
    @str.each_line("in") { |line| expected << line }
    assert_equal expected, actual
  end

  def test_iterate
    mmap = []
    @mmap.each_byte { |l| mmap << l }
    str = []
    @str.each_byte { |l| str << l }
    assert_equal(mmap, str, "<each_byte>")
  end

  def test_reg
    assert_equal(@str.scan(/include/), @mmap.scan(/include/), "<scan>")
    assert_equal(@mmap.index("rb_raise"), @mmap.index("rb_raise"), "<index>")
    assert_equal(@mmap.rindex("rb_raise"), @mmap.rindex("rb_raise"), "<rindex>")
    assert_equal(@mmap.index(/rb_raise/), @mmap.index(/rb_raise/), "<index>")
    assert_equal(@mmap.rindex(/rb_raise/), @mmap.rindex(/rb_raise/), "<rindex>")
    ("a".."z").each do |i|
      assert_equal(@mmap.index(i), @str.index(i), "<index>")
      assert_equal(@mmap.rindex(i), @str.rindex(i), "<rindex>")
      assert_equal(@mmap.index(i), @str.index(/#{i}/), "<index>")
      assert_equal(@mmap.rindex(i), @str.rindex(/#{i}/), "<rindex>")
    end
    @mmap.sub!(/GetMmap/, "XXXX")
    @str.sub!(/GetMmap/, "XXXX")
    assert_equal(@str, @mmap.to_str, "<after sub!>")
    @mmap.gsub!(/GetMmap/, "XXXX")
    @str.gsub!(/GetMmap/, "XXXX")
    assert_equal(@mmap.to_str, @str, "<after gsub!>")
    @mmap.gsub!(/YYYY/, "XXXX")
    @str.gsub!(/YYYY/, "XXXX")
    assert_equal(@mmap.to_str, @str, "<after gsub!>")
    assert_equal(@mmap.split(/\w+/), @str.split(/\w+/), "<split>")
    assert_equal(@mmap.split(/\W+/), @str.split(/\W+/), "<split>")
    assert_equal(@mmap.crypt("abc"), @str.crypt("abc"), "<crypt>")
  end

  def test_easy_sub!
    assert_equal(@mmap.index("rb_raise"), @mmap.index("rb_raise"), "<index>")
  end

  def test_modify
    internal_modify(:upcase!)
    internal_modify(:downcase!)
    internal_modify(:capitalize!)
    internal_modify(:swapcase!)
    internal_modify(:reverse!)
    internal_modify(:strip!)
    internal_modify(:chop!)
    internal_modify(:chomp!)
    internal_modify(:delete!, "A-Z")
    internal_modify(:squeeze!)
    skip "TODO"
    internal_modify(:tr!, "abcdefghi", "123456789")
    internal_modify(:tr_s!, "jklmnopqr", "123456789")
  end

  def test_clone
    assert_raises(TypeError) do
      @mmap.clone
    end
  end

  def test_dup
    assert_raises(TypeError) do
      @mmap.dup
    end
  end

  def test_comparison
    string = "azertyuiopqsdfghjklm"
    assert_kind_of(Mmap, mmap0 = Mmap.new("#{@tmp}/aa", "a"), "new a")
    File.open("#{@tmp}/bb", "w") { |f| f.puts "aaa" }
    assert_kind_of(Mmap, mmap1 = Mmap.new("#{@tmp}/bb", "w"), "new a")
    assert_equal(true, mmap0.empty?, "empty")
    assert_equal(true, mmap1.empty?, "empty")
    assert_equal(mmap0, mmap0 << string, "<<")
    assert_equal(mmap1, mmap1 << string, "<<")
    assert_equal(false, mmap0.empty?, "empty")
    assert_equal(false, mmap1.empty?, "empty")
    assert_equal(true, mmap0 == mmap1, "==")
    if string.respond_to?(:casecmp)
      assert_equal(0, mmap0.casecmp(string.upcase), "casecmp")
      assert_equal(0, mmap0.casecmp(mmap1), "casecmp")
    end
    assert_equal(true, mmap0 === mmap1, "===")
    assert_equal(false, mmap0 === string, "===")
    assert_equal(true, mmap0.eql?(mmap1), ".eql?")
    assert_equal(true, mmap1.eql?(mmap0), ".eql?")
    assert_equal(false, mmap1.eql?(string), ".eql?")
    assert_equal(mmap0.hash, mmap1.hash, "hash")
    assert_equal(true, mmap0.include?("azert"), "include")
    assert_equal(false, mmap1.include?("aqert"), "include")
    i = 0
    mmap0.scan(/./) { |c| assert_equal(c, string[i, 1], "scan"); i += 1 }
    assert_nil(mmap0.munmap, "munmap")
    assert_nil(mmap1.munmap, "munmap")
  end

  def test_protect
    assert_equal(@mmap, @mmap.protect("w"), "protect")
    assert_equal("a", @mmap[12] = "a", "affect")
    @str[12] = "a"
    assert_equal(@mmap.to_str, @str, "protect")
    assert_raises(TypeError) { @mmap << "a" }
    assert_equal(@mmap, @mmap.protect("r"), "protect")
    assert_raises(RuntimeError) { @mmap[12] = "a" }
    assert_raises(RuntimeError) { @mmap.protect("rw") }
  end

  def test_msync
    3.times do |i|
      [@mmap, @str].each { |l| l << "x" * 4096 }
      str = internal_read
      if str != @mmap.to_str
        @mmap.msync
        assert_equal(@mmap.to_str, internal_read, "msync")
        break
      end
    end
  end

  def test_frozen
    @mmap.freeze
    assert_raises FrozenError do
      @mmap.sub!(/GetMmap/, "XXXX")
      @str.sub!(/GetMmap/, "XXXX")
    end
  end

  def test_insert_frozen
    mmap = @mmap
    @mmap.freeze
    assert_raises FrozenError do
      mmap.insert(0, 1.chr)
    end
  end

  def test_count_and_sum
    assert_equal(@str.count("a"), @mmap.count("a"), "count single character")
    assert_equal(@str.count("aeiou"), @mmap.count("aeiou"), "count multiple characters")
    assert_equal(@str.count("^a"), @mmap.count("^a"), "count complement")
    assert_equal(@str.count("a-z"), @mmap.count("a-z"), "count range")

    assert_equal(@str.sum, @mmap.sum, "sum default")
    assert_equal(@str.sum(8), @mmap.sum(8), "sum with 8 bits")
    assert_equal(@str.sum(16), @mmap.sum(16), "sum with 16 bits")
    assert_equal(@str.sum(32), @mmap.sum(32), "sum with 32 bits")
  end

  def test_other
    test_comparison
    if File.exist?("#{@tmp}/aa")
      string = "azertyuiopqsdfghjklm"
      assert_kind_of(Mmap, mmap0 = Mmap.new("#{@tmp}/aa", "r"), "new r")
      assert_equal(string, mmap0.to_str, "content")
      assert_raises(RuntimeError) { mmap0[0] = 12 }
      assert_raises(RuntimeError) { mmap0 << 12 }
      assert_nil(mmap0.munmap, "munmap")
      assert_raises(ArgumentError) { Mmap.new(nil, "w") }
      assert_kind_of(Mmap, mmap0 = Mmap.new(nil, 12), "new w")
      assert_equal(false, mmap0.empty?, "empty")
      assert_equal("a", mmap0[0] = "a", "set")
      assert_raises(TypeError) { mmap0 << 12 }
      assert_nil(mmap0.advise(Mmap::MADV_DONTNEED), "advise")
      assert_equal("a", mmap0[0, 1], "get")
      assert_equal(mmap0, mmap0.sub!(/./) { "y" }, "sub")
      assert_equal(mmap0, mmap0.gsub!(/./) { "x" }, "gsub")
      assert_equal("x" * 12, mmap0.to_str, "retrieve")
      assert_equal("ab", mmap0[1..2] = "ab", "range")
      assert_raises(TypeError) { mmap0[1..2] = "abc" }
      assert_raises(ArgumentError) { mmap0.lock }
      assert_raises(ArgumentError) { Mmap::lockall(0) }
      assert_nil(mmap0.munmap, "munmap")
    end
  end

  private

  def assert_same_result(expected, actual, msg)
    expected.nil? ? assert_nil(actual, msg) : assert_equal(expected, actual, msg)
  end

  def internal_aset(a, b = nil, c = true)
    access = if b
               repl = String.new
               rand(12).times do
                 repl << (65 + rand(25))
               end
               if c
                 "[a, b] = \"#{repl}\""
               else
                 "[a..b] = \"#{repl}\""
               end
             else
               "[a] = #{(65 + rand(25))}.chr"
             end
    begin
      eval "@str#{access}"
    rescue IndexError, RangeError
      begin
        eval "@mmap#{access}"
      rescue IndexError, RangeError
      else
        flunk("*must* fail with IndexError")
      end
    else
      eval "@mmap#{access}"
    end
    assert_equal(@mmap.to_str, @str, "<internal aset>")
  end

  def internal_modify(idmod, *args)
    if res = @str.method(idmod)[*args]
      assert_equal(@mmap.method(idmod)[*args].to_str, res, "<#{idmod}>")
    else
      @mmap.method(idmod)[*args]
      assert_equal(@mmap.to_str, @str, "<#{idmod}>")
    end
  end

  def internal_read
    File.readlines(@mmap_c, nil)[0]
  end

  def internal_slice(a, b = nil, c = true)
    access = if b
               if c
                 ".slice!(a, b)"
               else
                 ".slice!(a..b)"
               end
             else
               ".slice!(a)"
             end
    begin
      eval "@str#{access}"
    rescue IndexError, RangeError
      begin
        eval "@mmap#{access}"
      rescue IndexError, RangeError
      else
        flunk("*must* fail with IndexError")
      end
    else
      eval "@mmap#{access}"
    end
    assert_equal(@mmap.to_str, @str, "<internal aset>")
  end
end
