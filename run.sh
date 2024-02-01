while true
do
  ./thread
  if [ $? != 0 ]; then
    echo "Program x encountered an error and stopped."
    break
  fi
done