## batterylogd

`batterylogd` is a small daemon that continuously logs battery and backlight information.
The data is stored in a CSV format that can be analysed further.

### The following CSV formats are used:

For batteries:
```
battery,battery_name,date,capacity,cycle_count,energy_full,energy_full_design,energy_now,power_now,present,status,voltage_min_design,voltage_now
```

For backlights:
```
backlight,backlight_name,date,max_brightness,brightness
```

Example data gathered on a Lenovo x270:
```
battery,BAT1,2018-01-18T20:12:09Z,5,341,21390000,23480000,1150000,0,1,Unknown,11400000,11004000
battery,BAT0,2018-01-18T20:12:09Z,87,183,21810000,23480000,18980000,3871000,1,Discharging,11400000,12136000
backlight,intel_backlight,2018-01-18T20:12:09Z,2210,6818
```

## Usage

```
Usage: batterylogd [options]

  -h   --help          Print this message and exit.
  -v   --version       Print version information and exit.
  -i   --interval      Sampling interval. Defaults to 30 seconds.
  -b   --battery       Path to a battery in sysfs. Argument can be specified
                       multiple times. If omitted entirely, automatic detection
                       will be enabled.
  -L   --backlight     Add entries for the display backlight to the log.
                       Argument can be specified multiple times. If omitted
                       entirely, automatic detection will be enabled.
  -l   --log           Path to log file. Defaults to $HOME/batterylogd.log.
```

## Building

Use the provided `Makefile`:

```
make
```

## Bugs

Please report them if you find any, I only have one device to test on.

