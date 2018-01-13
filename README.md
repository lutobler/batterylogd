## batterylogd

`batterylogd` is a small daemon that continuously logs battery information.
The data is stored in a CSV format that can then be analysed further.

The following CSV format is used:

```
battery_name,date,capacity,cycle_count,energy_full,energy_full_design,energy_now,power_now,present,status,voltage_min_design,voltage_now

```

* `battery_name` is the directory name in sysfs
* `date` is in ISO 8601 format. 
* The other data points correspond to the sysfs interface (e.g. `/sys/class/power_supply/BAT0`)

## Usage

```
Usage: batterylogd [options]

  -h   --help          Print this message and exit.
  -v   --version       Print version information and exit.
  -i   --interval      Sampling interval. Defaults to 30 seconds.
  -b   --battery       Path to a battery in sysfs. Argument can be specified
                       multiple times. If omitted entirely, automatic battery
                       detection will be enabled.
  -L   --backlight     Add entries for the display backlight to the log.
                       Fields for 'max_brightness' and 'current_brightness'
                       will be appended to the logged battery data.
  -l   --log           Path to log file. Defaults to $HOME/batterylogd.log.
```

## Building

Use the provided `Makefile`:

```
make
```

## Bugs

Please report them if you find any, I only have one device to test on.

