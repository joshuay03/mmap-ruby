# frozen_string_literal: true

require_relative "lib/mmap-ruby/version"

Gem::Specification.new do |spec|
  spec.name = "mmap-ruby"
  spec.version = MmapRuby::VERSION
  spec.authors = ["Guy Decoux", "Aaron Patterson", "Joshua Young"]
  spec.email = ["ts@moulon.inra.fr", "tenderlove@github.com", "djry1999@gmail.com"]

  spec.summary = "mmap wrapper for Ruby"
  spec.homepage = "https://github.com/joshuay03/mmap-ruby"
  spec.license = "https://www.ruby-lang.org/en/about/license.txt"
  spec.required_ruby_version = ">= 3.3.0"

  spec.metadata["documentation_uri"] = "https://joshuay03.github.io/mmap-ruby/"
  spec.metadata["source_code_uri"] = spec.homepage
  spec.metadata["changelog_uri"] = "#{spec.homepage}/blob/main/CHANGELOG.md"

  gemspec = File.basename(__FILE__)
  spec.files = IO.popen(%w[git ls-files -z], chdir: __dir__, err: IO::NULL) do |ls|
    ls.readlines("\x0", chomp: true).reject do |f|
      (f == gemspec) || f.start_with?(*%w[.github/ bin/ examples/ test/ .gitignore Gemfile])
    end
  end
  spec.require_paths = ["lib"]
  spec.extensions = ["ext/mmap_ruby/extconf.rb"]
end
