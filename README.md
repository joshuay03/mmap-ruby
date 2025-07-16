# MmapRuby

![Version](https://img.shields.io/gem/v/mmap-ruby)
![Build](https://img.shields.io/github/actions/workflow/status/joshuay03/mmap-ruby/.github/workflows/main.yml?branch=main)

[`mmap`](https://en.wikipedia.org/wiki/Mmap) wrapper for Ruby.

A modern fork of https://github.com/tenderlove/mmap.

**Docs:** https://joshuay03.github.io/mmap-ruby

**Example:**

```ruby
# frozen_string_literal: true

require "mmap-ruby"

PAGESIZE = 4096

file = File.open("aa", "w")
file.write("\0" * PAGESIZE)
file.write("test")
file.write("\0" * PAGESIZE)
file.close

mmap = Mmap.new("aa", "rw", offset: 0)
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
```

## Installation

Install the gem and add to the application's Gemfile by executing:

```bash
bundle add mmap-ruby
```

If bundler is not being used to manage dependencies, install the gem by executing:

```bash
gem install mmap-ruby
```

## Development

After checking out the repo, run `bin/setup` to install dependencies. Then, run `bundle exec rake` to run the tests.
You can also run `bin/console` for an interactive prompt that will allow you to experiment.

To install this gem onto your local machine, run `bundle exec rake install`. To release a new version, update the
version number in `version.rb`, and then run `bundle exec rake release`, which will create a git tag for the version,
push git commits and the created tag, and push the `.gem` file to [rubygems.org](https://rubygems.org).

## Contributing

Bug reports and pull requests are welcome on GitHub at https://github.com/[joshuay03]/mmap-ruby. This project is
intended to be a safe, welcoming space for collaboration, and contributors are expected to adhere to the
[code of conduct](https://github.com/[joshuay03]/mmap-ruby/blob/main/CODE_OF_CONDUCT.md).

## License

The gem is available as open source under the terms of the [Ruby license](https://www.ruby-lang.org/en/about/license.txt).

## Code of Conduct

Everyone interacting in the MmapRuby project's codebases, issue trackers, chat rooms and mailing lists is expected to
follow the [code of conduct](https://github.com/[joshuay03]/mmap-ruby/blob/main/CODE_OF_CONDUCT.md).
