# write a python program that list from 1 to 1000000000 in separate lines in a file called test.txt.

with open('/Users/mexiporc/Documents/GitHub/Computer-Network/Project2/starter_code/obj/test.txt', 'w') as file:
    for i in range(1, 1000001):
        file.write(str(i) + '\n')