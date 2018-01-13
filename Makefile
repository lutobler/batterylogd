TARGET = batterylogd
CPPFLAGS = -g -std=c++11 -Wall -Wextra -Wpedantic -lpthread

default: batterylogd

batterylogd:
	$(CXX) $(CPPFLAGS) -o $(TARGET) batterylogd.cc

clean:
	rm -f *.o $(TARGET)
