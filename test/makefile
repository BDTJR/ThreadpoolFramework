tar = test
flag = -g -std=c++11
lib = -lpthread  

test_file1 = ./test.cpp

src = ${test_file1}

${tar}: ${src}
	g++ ${flag} ${src} -o ${tar} ${lib}

.PRONY: exec clean

clean:
	@ rm ./${tar}

exec:
	@make clean
	@make
	@./${tar}