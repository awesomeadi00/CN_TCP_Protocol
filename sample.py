def write_numbers_to_file(n, filename="input_sample.txt"):
    """
    Create a text file and write numbers from 1 to n.

    Parameters:
    - n (int): The maximum number to write to the file.
    - filename (str): The name of the file to create (default: 'numbers.txt').
    """
    try:
        with open(filename, "w") as file:
            for i in range(1, n + 1):
                file.write(f"{i}\n")
        print(f"Successfully wrote numbers from 1 to {n} in '{filename}'")
    except Exception as e:
        print(f"An error occurred: {e}")


# Define the maximum number 'n'
n = 10000  # Replace with your desired value
write_numbers_to_file(n)
