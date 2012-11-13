default: index.fcgi

index.fcgi: index.cpp
	g++ index.cpp -lfcgi++ -lpqxx -o index.fcgi

clean:
	rm index.fcgi
