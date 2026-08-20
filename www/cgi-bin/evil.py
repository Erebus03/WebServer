#!/usr/bin/env python3
import sys
sys.stdout.write("Content-Type: text/plain\n")
sys.stdout.write("X-Evil: aaa\rInjected: yes\n")
sys.stdout.write("X Space: name-has-a-space\n")
sys.stdout.write(": empty-name\n")
sys.stdout.write("Status: 200 Not\rFound\n")
sys.stdout.write("\n")
sys.stdout.write("body-ok\n")
