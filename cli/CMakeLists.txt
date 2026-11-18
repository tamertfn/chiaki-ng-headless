
find_package(FFMPEG REQUIRED COMPONENTS avformat avcodec avutil)

set(SOURCE
		include/chiaki-cli.h
		src/discover.c
		src/wakeup.c
		src/stream.c)

add_library(chiaki-cli-lib STATIC ${SOURCE})
target_include_directories(chiaki-cli-lib PUBLIC "include")
target_link_libraries(chiaki-cli-lib chiaki-lib FFMPEG::avformat FFMPEG::avcodec FFMPEG::avutil)

if(CHIAKI_CLI_ARGP_STANDALONE)
	find_package(Argp REQUIRED)
	target_link_libraries(chiaki-cli-lib Argp::Argp)
endif()

add_executable(chiaki-cli src/main.c)
target_link_libraries(chiaki-cli chiaki-cli-lib)
install(TARGETS chiaki-cli)
