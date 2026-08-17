set(FASTTEXT_FOLDER "${PROJECT_SOURCE_DIR}/Dependencies/fastText")

add_library(fasttext STATIC
    "${FASTTEXT_FOLDER}/src/args.cc"
    "${FASTTEXT_FOLDER}/src/autotune.cc"
    "${FASTTEXT_FOLDER}/src/densematrix.cc"
    "${FASTTEXT_FOLDER}/src/dictionary.cc"
    "${FASTTEXT_FOLDER}/src/fasttext.cc"
    "${FASTTEXT_FOLDER}/src/loss.cc"
    "${FASTTEXT_FOLDER}/src/matrix.cc"
    "${FASTTEXT_FOLDER}/src/meter.cc"
    "${FASTTEXT_FOLDER}/src/model.cc"
    "${FASTTEXT_FOLDER}/src/productquantizer.cc"
    "${FASTTEXT_FOLDER}/src/quantmatrix.cc"
    "${FASTTEXT_FOLDER}/src/utils.cc"
    "${FASTTEXT_FOLDER}/src/vector.cc")
target_include_directories(fasttext PUBLIC "${FASTTEXT_FOLDER}/src")
target_compile_features(fasttext PUBLIC cxx_std_17)
set_target_properties(fasttext PROPERTIES FOLDER "Dependencies/")
