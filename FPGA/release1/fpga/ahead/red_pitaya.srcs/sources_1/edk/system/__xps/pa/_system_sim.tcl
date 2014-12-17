######################################################
#
# XPS Tcl API script generated by PlanAhead
#
######################################################

proc _main_ {} {
  cd "/home/matej/WORK/red_pitaya/redpitaya_svn/FPGA/release1/fpga/ahead/red_pitaya.srcs/sources_1/edk/system"
  if { [ catch {xload xmp system.xmp} result ] } {
    exit 10
  }

  # Set host application type
  xset intstyle PA

  # Set design flow type
  xset flow ise

  # Set language type
  xset hdl verilog

  # Set target simulator
  xset simulator isim

  # Set simulation flow
  xset sim_model behavioral

  # Clean Simulation Models
  if { [catch {run simclean} result] } {
    return -1
  }

  # Launch Simulation Models generation
  if { [catch {run simmodel} result] } {
    return -1
  }
  return $result
}

# Generate Simulation Models
if { [catch {_main_} result] } {
  exit -1
}

# Check return status and exit
if { [string length $result] == 0 } {
  exit 0
} else {
  exit $result
}
