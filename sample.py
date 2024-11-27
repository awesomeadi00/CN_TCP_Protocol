def write_numbers_to_file(n, filename="input_sample.txt"):
    try:
        with open(filename, "w") as file:
            for i in range(1, n + 1):
                file.write(f"{i}\n")
        print(f"Successfully wrote numbers from 1 to {n} in '{filename}'")
    except Exception as e:
        print(f"An error occurred: {e}")

n = 2000  
write_numbers_to_file(n)
