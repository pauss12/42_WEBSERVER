The Decision Tree

Longest prefix match rule!!

Does the folder actually exist on the hard drive?

No: Return 404 Not Found.

Yes: Proceed to step 2.

Does the location block have an index directive?

Yes: The server loops through the index files provided (e.g., index.html, default.html). It appends each one to the folder path and checks if it exists.

If one exists, serve it (e.g., return /var/www/res/index.html).

If none of them exist, proceed to step 3.

No (or empty): Proceed to step 3.

Does the location block have autoindex on;?

Yes: The server reads the contents of the directory (using opendir() and readdir() in C++), dynamically builds an HTML page with clickable links to the files/folders inside, and sends that HTML to the client.

No (or autoindex off;): Proceed to step 4.

The Ultimate Fallback:

Return 403 Forbidden.