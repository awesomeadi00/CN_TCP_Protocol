# Computer Networks: Project 2

- Aditya Pandhare: ap7146
- Kevin Chu: kc4624

## How to execute: 

> - Note: Make sure you have your input files ready.
> - There is a `sample.py` file which you can execute to create an `input_sample.txt` Simply move this file into the `..obj/` directory

1. First compile by going into `cd code/` and type `make`

2. Then go to `cd ../obj` on two sepearte terminals

3. First start `receiver` and execute: 
```
Reciever structure: ./rdt_receiver <PORT_NO> <OUTPUT_FILE>

./rdt_receiver 5454 output_sample.txt
```

4. Then in a seperate terminal, execute the `sender`: 
```
Sender structure: ./rdt_receiver <HOSTNAME> <PORT_NO> <INPUT_FILE>

./rdt_sender $MAHIAMAHI_BASE 5454 input_sample.txt

OR LOCALLY: 
./rdt_sender 127.0.0.1 5454 input_sample.txt
```