# Default target
all: rdt_sender rdt_receiver

# Rule to compile rdt_sender
rdt_sender: code/rdt_sender.c
	gcc code/rdt_sender.c code/packet.c code/common.c -o rdt_sender

# Rule to compile rdt_receiver
rdt_receiver: code/rdt_receiver.c
	gcc code/rdt_receiver.c code/packet.c code/common.c -o rdt_receiver

# Clean target
clean:
	rm -f rdt_sender rdt_receiver
