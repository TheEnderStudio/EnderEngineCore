#include <gtest/gtest.h>

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	printf("Running all tests ...");
	return RUN_ALL_TESTS();
}