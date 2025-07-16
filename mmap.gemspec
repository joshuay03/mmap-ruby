# frozen_string_literal: true

require_relative "lib/mmap-ruby/version"

Gem::Specification.new do |spec|
  spec.name = "mmap-ruby"
  spec.version = MmapRuby::VERSION
  spec.authors = ["Guy Decoux", "Aaron Patterson", "Joshua Young"]
  spec.email = ["ts@moulon.inra.fr", "tenderlove@github.com", "djry1999@gmail.com"]

  spec.summary = "The Mmap class implement memory-mapped file objects"
  spec.homepage = "https://github.com/joshuay03/mmap-ruby"
  spec.license = "https://www.ruby-lang.org/en/about/license.txt"
  spec.required_ruby_version = ">= 3.3.0"

  spec.metadata["source_code_uri"] = spec.homepage
  spec.metadata["changelog_uri"] = "#{spec.homepage}/blob/main/CHANGELOG.md"

  spec.files = Dir["lib/**/*", "ext/**/*", "**/*.{gemspec,md,txt}"]
  spec.require_paths = ["lib"]
  spec.extensions = ["ext/mmap_ruby/extconf.rb"]
end
