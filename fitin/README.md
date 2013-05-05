# FITIn - A simple fault injection tool

## About

FITIn is a tool to be used by Valgrind that allows flipping a bit of
data at a certain time.

## Accesses and Flips

In order to understand when and how FITIn is doing bit flips, please
read the following information carefully.

In the source code, you specify which start addresses you want
to monitor inside a selected function (default: ```main```).

Whenever FITIn detects a read from a memory beginning at a monitored
start address, a counter is increased - even if the data has been loaded
into a register.

If the counter - by whatever monitored has been accessed - hits
```--mod-load-time``, it will attempt to flip the bit ```--mod-bit```
before the actual read. 'Attempt' in terms of evaluating whether the
desired bit is in range of the loaded data. If not, no flip can occur.
The desired bit counts from the offset of the least significant bit of
the variable.

By not discriminating between registers and memory, a flipped value may
be dropped if it resides in a register and won't be written into its
original location afterwards. This may be the case if you have memory
you only need to read from. To persist the manipulated value in any
case, please use ```--persist-flip=yes```. With this option, even if the
value could not be flipped at first (e.g. only the first byte of an
```int``` was loaded), it will replace the flipped byte-of-the-bit in
the memory.

FITIn will also treat reads of memory and registers by syscalls.

To get a better feeling for 'accesses', think of your C code while it
is not optimized and spot any reading from a monitored variable.

Things that do not count as access (with ```a``` being monitored):

* Copies: ```b = a;```, the copy is one access to ```a``` but any access
  to ```b``` is out of scope.
* Identity assignments: ```a=a;```
* Anonymous expressions: ```(a + 1) * 2```, again there is only one
  access before the addition.
* Irregular access: ```((char*)&a) + 1)```, as this is not touching the
  start address at execution time (but a is wide enough, e.g. by
  ```int a;```).

## Use

FITIn requires annotating source code in order to work as expected.

### Code

Please include ```fi_client.h``` from the Valgrind include-directory.
For setting up monitoring of access to memory, use one of the following
macros:

* FITIN_MONITOR_VARIABLE(var)
* FITIN_MONITOR_ADDRESS(ptr, size)

Then compile the source code. Deactivating any optimization levels is
strongly recommended, as optimazations by the compiler may render this
tool useless.

### Command line

Start FITIn by specifying ```--tool=fitin``` followed by options and the
program to run.

The most important options are ```--mod-load-time=n``` that will trigger
the flip at the n-th access to a monitored memory area at the m-th bit
specified by ```--mod-bit=m``` (with 0 being the LSB).

For additional options of Valgrind and FITIn, please consult
```--tool=fitin --help```.

By increasing the verbosity Level of Valgrind, you will also receive
additional information about FITIn.

### Examples, Tests

Have a look at the ```tests``` folder. Please not that the tests are not
build by the ordinary ```make``` command and require Ruby if used by the
test suite. 

For more information, please consult the ```README``` inside.

## Limitations

* One fault injection per run.
* Monitoring across functions, only one per run (```--fnname```).
* If accessing data from memory, the tool focuses on matching start
  addresses being monitored. For uses of different alignments, use
  ```FITIN_MONITOR_ADDRESS``` instead of ```FITIN_MONITOR_VARIABLE```
  for every byte that may be a start address.
* Limited support for rotating register files: not respected for IRDirty
  helpers (some special instructions) and syscalls that read from
  those registers.

## Reporting of Bugs, Feature Requests, Support

Please post anything like that to
https://github.com/MarcelHB/valgrind-fitin/issues
