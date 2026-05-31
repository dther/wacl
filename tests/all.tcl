# wacl test driver.
#
# Sources every *.test file in this directory, in lexical order, in the
# same interpreter. We don't use tcltest::runAllTests because it spawns
# a child interp per test file — child interps don't share our wacl-*
# packages (or the host-granted `eval`) with the parent, so each test
# file would have to redo the bootstrap from scratch and the JS-side
# eval grant wouldn't survive at all.
#
# Individual .test files do NOT call cleanupTests, because that resets
# ::tcltest::numTests and the runner page reads those counts after
# everything's done. Manual summary at the end here.

package require tcltest

# Show passes as well as failures, plus skips. Default verbosity hides
# successes, which is fine for CI but undersells progress in a live
# log; you want to see the tests ticking past.
tcltest::configure -verbose {pass skip error}

set scriptDir [file dirname [file normalize [info script]]]

foreach f [lsort [glob -nocomplain -directory $scriptDir *.test]] {
    puts ""
    puts "---- [file tail $f] ----"
    source $f
}

puts ""
puts "==== summary ===="
puts "  total:   $::tcltest::numTests(Total)"
puts "  passed:  $::tcltest::numTests(Passed)"
puts "  failed:  $::tcltest::numTests(Failed)"
puts "  skipped: $::tcltest::numTests(Skipped)"
