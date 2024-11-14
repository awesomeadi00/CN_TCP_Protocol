# write a python program that list from 1 to 1000000000 in separate lines in a file called test.txt.

with open(
    "/Users/awesomeadi00/Desktop/Computer Networks/Projects/Project 2 - TCP/CN_P2A_TCP/input.txt",
    "w",
) as file:
    for i in range(1, 1000001):
        file.write(str(i) + '\n')
