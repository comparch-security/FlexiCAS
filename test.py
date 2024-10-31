#!/usr/bin/python3


import os.path
import subprocess


# configuration type

def run_tests(test_base, c):
    for i in c:
        s_arg    = "%d" % (i)
        print(test_base + " " + s_arg)
        subprocess.call(test_base + " " + s_arg + " >> " + "performance/mul.log", shell=True)

run_tests("performance/multi-l2-msi", range(1, 5, 1))
run_tests("performance/multi-l3-msi", range(1, 5, 1))