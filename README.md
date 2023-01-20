# **MultiThreaded HTTP Server**
httpserver is a simple web server that allows for three basic http methods: GET, PUT, and APPEND. httpserver processes the requests and logs them to either a designated file or stdout. httpserver using multiple threads to speed up and handle multiple operations at once and also preserves atomicity and coherency.

### **Usage:**
`./httpserver: [-t threads] [-l logfile] <port>`

### **Design:**
httpserver is split into two mail files along with a data structure file. The main httpserver.c file contains the main function and the functions that handle the http connection. The methods.c along with the methods.h file contain the functions that handle the http methods, including GET, PUT, and APPEND as well as the functions to read and write to the file and send different responses. The httpserver.c file accepts a connection from the client, parses through the request data to get the accurate method requested and the Content-Length, and then calls the appropriate method function. It also checks that the request is in the correct format and sends the appropriate error is the request is bad. The data structure files (queue.c and queue.h) contain the functions to handle the queue and the queue itself. The queue is used to handle the requests without deadlocks or busy waiting. The queue is implemented using a linked list. To maintain atomicity and coherency httpserver uses tempfiles to store the data before writing to the main file. The tempfiles are deleted after the request is processed.


### **Responses:**
200 OK: The request was successful.
201 Created: The request was successful and the resource was created.
403 Forbidden: The request was not allowed.
404 Not Found: The requested file was not found.
500 Internal Server Error: The server encountered an error.

### **Data Structures:**
httpserver uses one main data structure, a work queue

#### **Queue:**
The queue is used to store the requests that are waiting to be processed. The queue is implemented using a linked list of nodes that hold request data such as "connfd" and "requestState". This is used such that a request can be enqueued at any point and resumed later by a different thread.

## Authors

* **[Will Hord](https://github.com/WillHord)** 

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details
