# frozen_string_literal: true

require "bundler/gem_tasks"
require "minitest/test_task"
require "rake/extensiontask"

GEMSPEC = Gem::Specification.load("mmap-ruby.gemspec")

Minitest::TestTask.create

Rake::ExtensionTask.new("mmap_ruby", GEMSPEC) do |ext|
  ext.lib_dir = "lib/mmap-ruby"
end

task build: :compile
task default: %i[clobber compile test]
