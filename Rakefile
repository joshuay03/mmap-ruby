# frozen_string_literal: true

require "bundler/gem_tasks"
require "minitest/test_task"

Minitest::TestTask.create

require "rake/extensiontask"

task build: :compile

GEMSPEC = Gem::Specification.load("mmap_ruby.gemspec")

Rake::ExtensionTask.new("mmap_ruby", GEMSPEC) do |ext|
  ext.lib_dir = "lib/mmap-ruby"
end

task default: %i[clobber compile test]
