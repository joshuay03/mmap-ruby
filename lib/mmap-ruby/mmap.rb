# frozen_string_literal: true

module MmapRuby
  class Mmap
    include Comparable
    include Enumerable

    def clone # :nodoc:
      raise TypeError, "can't clone instance of #{self.class}"
    end

    def dup # :nodoc:
      raise TypeError, "can't dup instance of #{self.class}"
    end

    # See https://docs.ruby-lang.org/en/master/String.html#method-i-each_byte
    def each_byte(...)
      to_str.each_byte(...)
    end
    alias :each :each_byte

    # See https://docs.ruby-lang.org/en/master/String.html#method-i-each_line
    def each_line(...)
      to_str.each_line(...)
    end

    # See https://docs.ruby-lang.org/en/master/String.html#method-i-scan
    def scan(...)
      to_str.scan(...)
    end

    private

    def process_options(options)
      options.each do |key, value|
        key_str = key.to_s
        case key_str
        when "initialize" # skip
        when "length" then set_length value
        when "offset" then set_offset value
        when "advice" then set_advice value
        when "increment" then set_increment value
        when "ipc" then set_ipc value
        else raise TypeError, "unknown option #{key_str}"
        end
      end
    end
  end
end
